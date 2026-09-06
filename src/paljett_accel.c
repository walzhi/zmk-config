/*
 * Paljett - styrplatta i absolutlage for ZMK
 *
 * Plattan rapporterar var fingret ligger. Modulen raknar fram rorelsen
 * ur skillnaden mellan tva positioner och skriver om handelserna till
 * relativa, sa att iOS ser en vanlig mus.
 *
 * Tre handelser kommer per avlasning: ABS_X, ABS_Y och ABS_Z. Z bar
 * sync-flaggan. De skrivs om till REL_X, REL_Y och BTN_0. Ligger fingret
 * i skrollzonen blir Y-platsen REL_WHEEL i stallet.
 *
 * Fingerlyft nollstaller sparningen, annars skulle markoren kastas tvars
 * over skarmen nar du tar om greppet.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <drivers/input_processor.h>

LOG_MODULE_REGISTER(paljett_accel, CONFIG_ZMK_LOG_LEVEL);

#define PALJETT_NOD DT_NODELABEL(paljett_accel)

/* Faktorn ar i tusendelar. */
#define ENHET 1000

#define KURV_POTENS DT_PROP_OR(PALJETT_NOD, curve_power, 3)
#define TROSKEL DT_PROP_OR(PALJETT_NOD, release_threshold, ENHET)
#define HALL_MAX_US DT_PROP_OR(PALJETT_NOD, hold_max_us, 24000)

#define DT_GOLV_US 4000
#define DT_TAK_US 60000
#define UTJAMNING 16
#define UT_TAK 2047

/* Fingret nere respektive uppe. Z gar fran 0 till 63. Hysteres sa att
   ett finger som vilar latt inte flimrar mellan lagena. */
#define Z_NERE 8
#define Z_UPPE 3

/* Rorelser kortare an sa har manga steg raknas som darr och kastas.
   Hoj om markoren kryper nar du haller stilla. */
#define DODZON 3

/* Prov som slangs direkt efter nedsattning. Det forsta ar ofta skevt. */
#define INKORNING 2

/* Skrollzonen i X-led. Plattan ar ungefar 0 till 2047 bred. Visar sig
   zonen ligga pa fel sida, byt till 0 och 500. */
#define SKROLL_MIN_X 1500
#define SKROLL_MAX_X 2047

/* Steg i Y-led per hack pa hjulet. Lagre ger snabbare skroll. */
#define SKROLL_STEG 90

/* Satt 1 for att vanda skrollriktningen. */
#define SKROLL_VAND 0

/* En beroring kortare an sa har, med kortare vandring an sa har,
   raknas som en tapp. */
#define TAPP_MAX_MS 220
#define TAPP_MAX_ROR 120

/* Antal prov som knappen halls nere, ungefar tio millisekunder per prov. */
#define KLICK_PROV 5

/* Fonster for dubbel- och trippelklick. */
#define KLICK_FONSTER_MS 250

/* Bordslaget speglar plattan. Satt 1 pa Y ocksa om du vrider ett halvt
   varv i stallet for att vanda den. */
#define BORDSLAGE_VAND_X 1
#define BORDSLAGE_VAND_Y 0

struct paljett_konfig {
    int32_t min_faktor;
    int32_t max_faktor;
    int32_t fart_max;
};

struct paljett_data {
    int32_t fart;
    int32_t ack_x;
    int32_t ack_y;
    int64_t prov_us;
    int64_t hall_us;

    int32_t prov_x;
    int32_t prov_y;
    int32_t prov_z;

    int32_t forra_x;
    int32_t forra_y;
    bool har_forra;
    bool fingret_nere;
    uint8_t inkorning;

    int64_t nere_us;
    int32_t vandring;
    bool skrollzon;

    int32_t ack_hjul;

    uint8_t klick_rakning;
    int64_t klick_us;
    uint8_t klick_kvar;
    bool klick_vantar;

    int32_t ut_x;
    int32_t ut_y;
    int32_t ut_hjul;
    uint8_t ut_knapp;
    bool ut_skroll;

    bool bordslage;
};

static inline int32_t belopp(int32_t v) { return v < 0 ? -v : v; }

static inline int32_t vektorlangd(int32_t x, int32_t y) {
    int32_t a = belopp(x);
    int32_t b = belopp(y);
    int32_t stor = MAX(a, b);
    int32_t liten = MIN(a, b);

    return stor + (liten * 3) / 8;
}

static inline int32_t avrunda(int32_t tusendelar) {
    if (tusendelar >= 0) {
        return (tusendelar + ENHET / 2) / ENHET;
    }

    return -((-tusendelar + ENHET / 2) / ENHET);
}

static int32_t kurva(const struct paljett_konfig *k, int32_t fart) {
    if (fart >= k->fart_max) {
        return k->max_faktor;
    }
    if (fart <= 0) {
        return k->min_faktor;
    }

    int32_t t = (int32_t)(((int64_t)fart * ENHET) / k->fart_max);
    int32_t tp = t;

    for (int i = 1; i < KURV_POTENS; i++) {
        tp = (int32_t)(((int64_t)tp * t) / ENHET);
    }

    return k->min_faktor + (int32_t)(((int64_t)(k->max_faktor - k->min_faktor) * tp) / ENHET);
}

/* Forsta klicket skickas direkt. Andra halls tillbaka tills fonstret
   gatt ut. Kommer ett tredje kastas det andra och laget slas om. */
static void hantera_tapp(struct paljett_data *d, int64_t nu) {
    d->klick_us = nu;

    if (d->klick_rakning == 0) {
        d->klick_rakning = 1;
        d->klick_kvar = KLICK_PROV;
    } else if (d->klick_rakning == 1) {
        d->klick_rakning = 2;
        d->klick_vantar = true;
    } else {
        d->klick_rakning = 0;
        d->klick_vantar = false;
        d->bordslage = !d->bordslage;
        LOG_DBG("bordslage %d", (int)d->bordslage);
    }
}

static void behandla_prov(struct paljett_data *d, const struct paljett_konfig *k) {
    int64_t nu = k_ticks_to_us_floor64(k_uptime_ticks());
    int32_t x = d->prov_x;
    int32_t y = d->prov_y;
    int32_t z = d->prov_z;

    if (d->klick_rakning != 0 && (nu - d->klick_us) >= ((int64_t)KLICK_FONSTER_MS * 1000)) {
        if (d->klick_vantar) {
            d->klick_vantar = false;
            d->klick_kvar = KLICK_PROV;
        }
        d->klick_rakning = 0;
    }

    bool nere = d->fingret_nere ? (z > Z_UPPE) : (z >= Z_NERE);

    if (nere && !d->fingret_nere) {
        d->fingret_nere = true;
        d->har_forra = false;
        d->inkorning = INKORNING;
        d->nere_us = nu;
        d->vandring = 0;
        d->skrollzon = (x >= SKROLL_MIN_X && x <= SKROLL_MAX_X);
        d->ack_x = 0;
        d->ack_y = 0;
        d->ack_hjul = 0;
        d->fart = 0;
        d->prov_us = nu;
        d->hall_us = nu;

        LOG_DBG("ned x %d y %d z %d skroll %d", x, y, z, (int)d->skrollzon);
    } else if (!nere && d->fingret_nere) {
        d->fingret_nere = false;
        d->har_forra = false;

        int64_t langd_ms = (nu - d->nere_us) / 1000;

        LOG_DBG("upp %d ms vandring %d", (int)langd_ms, d->vandring);

        if (!d->skrollzon && langd_ms <= TAPP_MAX_MS && d->vandring <= TAPP_MAX_ROR) {
            hantera_tapp(d, nu);
        }
    }

    int64_t dt_us = nu - d->prov_us;
    d->prov_us = nu;
    dt_us = CLAMP(dt_us, DT_GOLV_US, DT_TAK_US);

    int32_t dx = 0;
    int32_t dy = 0;

    if (nere) {
        if (d->inkorning > 0) {
            d->inkorning--;
            d->forra_x = x;
            d->forra_y = y;
            d->har_forra = true;
        } else if (d->har_forra) {
            dx = x - d->forra_x;
            dy = y - d->forra_y;
            d->forra_x = x;
            d->forra_y = y;
        } else {
            d->forra_x = x;
            d->forra_y = y;
            d->har_forra = true;
        }
    }

    if (vektorlangd(dx, dy) < DODZON) {
        dx = 0;
        dy = 0;
    }

    d->vandring += vektorlangd(dx, dy);

    if (d->skrollzon) {
        d->ut_skroll = true;

        d->ack_hjul += SKROLL_VAND ? dy : -dy;

        while (d->ack_hjul >= SKROLL_STEG) {
            d->ut_hjul += 1;
            d->ack_hjul -= SKROLL_STEG;
        }
        while (d->ack_hjul <= -SKROLL_STEG) {
            d->ut_hjul -= 1;
            d->ack_hjul += SKROLL_STEG;
        }
    } else {
        d->ut_skroll = false;

        int32_t momentan = (int32_t)(((int64_t)vektorlangd(dx, dy) *
