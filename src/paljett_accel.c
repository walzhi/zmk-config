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

/* Plattans mitt i absoluta koordinater. Las av i loggen och justera. */
#define MITT_X 1024
#define MITT_Y 768

/* X och Y har olika antal steg per millimeter. Skalorna gor dem jamforbara
   sa att vinkeln blir riktig. */
#define X_SKALA 3
#define Y_SKALA 4

/* Fingret maste landa langre ut an sa har for att draget ska bli skroll.
   Plattans ytterkant ligger kring 3000 i den har skalan. */
#define SKROLL_RADIE 2000

/* 0 hela ringen, 1 bara hoger sida, -1 bara vanster sida. Visar det sig
   att zonen hamnat pa fel sida, byt tecken. */
#define SKROLL_SIDA 1

/* Hur hogt upp och ner zonen stracker sig fran plattans mitt. Kanten
   ligger kring 3000, sa 1536 ger en zon som tacker halva hojden. */
#define SKROLL_MAX_NY 1536

/* Vinkel per hack pa hjulet, i tusendels radianer. Ett helt varv ar 6283,
   sa 180 ger ungefar trettiofem hack per varv. Lagre ger snabbare skroll. */
#define SKROLL_STEG_MRAD 180

/* Vinkelandringar storre an sa har kastas som orimliga. */
#define SKROLL_MAX_MRAD 800

/* Narmare mitten an sa har blir vinkeln for brusig for att anvandas. */
#define SKROLL_MIN_RADIE 700

/* Utjamning av vinkelfarten. Hogre ger mjukare men trogare skroll. */
#define SKROLL_UTJAMNING 4

/* Hogsta antal hack per avlasning. Taket hindrar att en ryckig avlasning
   dumpar flera hack pa en gang, vilket kanns som ett hopp. */
#define SKROLL_MAX_PER_PROV 1

/* Satt 0 for att vanda skrollriktningen tillbaka. */
#define SKROLL_VAND 1

/* En beroring kortare an sa har, med kortare vandring an sa har,
   raknas som en tapp. */
#define TAPP_MAX_MS 220
#define TAPP_MAX_ROR 120

/* Antal prov som knappen halls nere, ungefar tio millisekunder per prov. */
#define KLICK_PROV 5

/* Fonster for dubbel- och trippelklick. */
#define KLICK_FONSTER_MS 250

/* Dampning av tvarrorelse. Nar ett drag ar tydligt vagratt eller lodratt
   skalas den vinkelrata komponenten ned, sa att bagen fingret gor kring
   knogleden inte syns. Sneda drag ror den inte, eftersom dampningen bara
   slar till nar en axel klart dominerar, och den trappas in mjukt sa att
   markoren aldrig kanns fastlast i ett rutnat.

   RAK_GRANS: den mindre axelns andel av den storre, i tusendelar. Ligger
   andelen over det har raknas draget som snett och dampas inte.
   RAK_MIN: hur lite som blir kvar av tvarrorelsen nar dampningen ar som
   starkast. 1000 stanger av dampningen helt.
   RAK_MIN_FART: langsammare drag an sa har dampas inte alls, sa att
   finjustering inte motarbetas. 0 dampar alltid. */
#define RAK_GRANS 450
#define RAK_MIN 350
#define RAK_MIN_FART 400

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

    int32_t rikt_x;
    int32_t rikt_y;

    int32_t ack_hjul;
    int32_t hjul_fart;

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
        int32_t ned_nx = (x - MITT_X) * X_SKALA;
        int32_t ned_ny = (y - MITT_Y) * Y_SKALA;
        int64_t ned_r2 = (int64_t)ned_nx * ned_nx + (int64_t)ned_ny * ned_ny;

        bool i_ringen = ned_r2 >= ((int64_t)SKROLL_RADIE * SKROLL_RADIE);
        bool ratt_sida = (SKROLL_SIDA == 0) || (SKROLL_SIDA > 0 ? (ned_nx > 0) : (ned_nx < 0));
        bool ratt_hojd = belopp(ned_ny) <= SKROLL_MAX_NY;

        /* Zonen provas bara vid nedsattning. Har draget val borjat som
           skroll fortsatter det vara skroll hela varvet runt, oavsett var
           pa plattan fingret hamnar. */
        d->skrollzon = i_ringen && ratt_sida && ratt_hojd;
        d->ack_x = 0;
        d->ack_y = 0;
        d->ack_hjul = 0;
        d->hjul_fart = 0;
        d->fart = 0;
        d->rikt_x = 0;
        d->rikt_y = 0;
        d->prov_us = nu;
        d->hall_us = nu;

        LOG_DBG("ned x %d y %d z %d radie %d skroll %d", x, y, z,
                vektorlangd(ned_nx, ned_ny), (int)d->skrollzon);
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

    int32_t nx1 = 0;
    int32_t ny1 = 0;
    int32_t nx2 = 0;
    int32_t ny2 = 0;
    bool har_vinkel = false;

    if (nere) {
        nx2 = (x - MITT_X) * X_SKALA;
        ny2 = (y - MITT_Y) * Y_SKALA;

        if (d->inkorning > 0) {
            d->inkorning--;
            d->forra_x = x;
            d->forra_y = y;
            d->har_forra = true;
        } else if (d->har_forra) {
            nx1 = (d->forra_x - MITT_X) * X_SKALA;
            ny1 = (d->forra_y - MITT_Y) * Y_SKALA;
            har_vinkel = true;

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

        /* Vinkeln kring plattans mitt, inte rorelsen i hojdled. Korsprodukten
           delad med skalarprodukten ar tangens for vinkelandringen, och for
           sma vinklar ar det vinkeln sjalv i tusendels radianer. Darfor
           fortsatter skrollen at samma hall hela varvet runt. */
        if (har_vinkel) {
            int64_t kryss = (int64_t)nx1 * ny2 - (int64_t)ny1 * nx2;
            int64_t prick = (int64_t)nx1 * nx2 + (int64_t)ny1 * ny2;
            int32_t mrad = 0;

            if (prick > ((int64_t)SKROLL_MIN_RADIE * SKROLL_MIN_RADIE)) {
                int32_t matt = (int32_t)((kryss * 1000) / prick);

                if (matt > -SKROLL_MAX_MRAD && matt < SKROLL_MAX_MRAD) {
                    mrad = matt;
                }
            }

            /* Vinkelfarten jamnas ut sa att enstaka skakiga avlasningar
               inte syns som hack. Utjamningen tappar ingen vinkel, den
               fordelar den bara over nagra fler avlasningar. */
            d->hjul_fart = (d->hjul_fart * (SKROLL_UTJAMNING - 1) + mrad) / SKROLL_UTJAMNING;

            d->ack_hjul += SKROLL_VAND ? -d->hjul_fart : d->hjul_fart;
            d->ack_hjul = CLAMP(d->ack_hjul, -SKROLL_STEG_MRAD * 4, SKROLL_STEG_MRAD * 4);
        }

        int32_t hack = 0;

        while (d->ack_hjul >= SKROLL_STEG_MRAD && hack < SKROLL_MAX_PER_PROV) {
            hack++;
            d->ack_hjul -= SKROLL_STEG_MRAD;
        }
        while (d->ack_hjul <= -SKROLL_STEG_MRAD && hack > -SKROLL_MAX_PER_PROV) {
            hack--;
            d->ack_hjul += SKROLL_STEG_MRAD;
        }

        d->ut_hjul += hack;
    } else {
        d->ut_skroll = false;

        int32_t momentan = (int32_t)(((int64_t)vektorlangd(dx, dy) * 1000000) / dt_us);

        momentan = MIN(momentan, k->fart_max * 2);

        d->fart = (d->fart * (UTJAMNING - 1) + momentan) / UTJAMNING;

        /* Dragets riktning jamnas over flera prov. Ett enskilt prov ar for
           litet och for brusigt for att avgora om draget ar axelrikt. */
        d->rikt_x = (d->rikt_x * 7 + belopp(dx) * 100) / 8;
        d->rikt_y = (d->rikt_y * 7 + belopp(dy) * 100) / 8;

        if (RAK_MIN < 1000 && d->fart >= RAK_MIN_FART) {
            int32_t stor = MAX(d->rikt_x, d->rikt_y);
            int32_t liten = MIN(d->rikt_x, d->rikt_y);

            if (stor > 0) {
                int32_t andel = (int32_t)(((int64_t)liten * 1000) / stor);

                if (andel < RAK_GRANS) {
                    /* Full dampning nar andelen ar noll, ingen alls vid
                       gransen, och en rak linje daremellan. */
                    int32_t kvar = RAK_MIN + (((1000 - RAK_MIN) * andel) / RAK_GRANS);

                    if (d->rikt_x < d->rikt_y) {
                        dx = (int32_t)(((int64_t)dx * kvar) / 1000);
                    } else {
                        dy = (int32_t)(((int64_t)dy * kvar) / 1000);
                    }
                }
            }
        }

        int32_t faktor = kurva(k, d->fart);

        if (d->bordslage) {
            if (BORDSLAGE_VAND_X) {
                dx = -dx;
            }
            if (BORDSLAGE_VAND_Y) {
                dy = -dy;
            }
        }

        d->ack_x += dx * faktor;
        d->ack_y += dy * faktor;

        bool nog_lang = ((int64_t)d->ack_x * d->ack_x + (int64_t)d->ack_y * d->ack_y) >=
                        ((int64_t)TROSKEL * TROSKEL);
        bool tiden_ute = (nu - d->hall_us) >= HALL_MAX_US;

        if (nog_lang || tiden_ute) {
            int32_t ux = avrunda(d->ack_x);
            int32_t uy = avrunda(d->ack_y);

            d->ack_x -= ux * ENHET;
            d->ack_y -= uy * ENHET;

            d->ut_x += ux;
            d->ut_y += uy;
            d->hall_us = nu;
        }
    }

    if (d->klick_kvar > 0) {
        d->klick_kvar--;
        d->ut_knapp = 1;
    } else {
        d->ut_knapp = 0;
    }
}

static int paljett_hantera(const struct device *dev, struct input_event *handelse, uint32_t param1,
                           uint32_t param2, struct zmk_input_processor_state *tillstand) {
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    ARG_UNUSED(tillstand);

    struct paljett_data *d = dev->data;
    const struct paljett_konfig *k = dev->config;

    if (handelse->type != INPUT_EV_ABS) {
        return 0;
    }

    if (handelse->code == INPUT_ABS_X) {
        d->prov_x = handelse->value;

        handelse->type = INPUT_EV_REL;
        handelse->code = INPUT_REL_X;

        if (d->ut_skroll) {
            handelse->value = 0;
        } else {
            handelse->value = CLAMP(d->ut_x, -UT_TAK, UT_TAK);
            d->ut_x -= handelse->value;
        }
    } else if (handelse->code == INPUT_ABS_Y) {
        d->prov_y = handelse->value;

        handelse->type = INPUT_EV_REL;

        if (d->ut_skroll) {
            handelse->code = INPUT_REL_WHEEL;
            handelse->value = CLAMP(d->ut_hjul, -UT_TAK, UT_TAK);
            d->ut_hjul -= handelse->value;
        } else {
            handelse->code = INPUT_REL_Y;
            handelse->value = CLAMP(d->ut_y, -UT_TAK, UT_TAK);
            d->ut_y -= handelse->value;
        }
    } else if (handelse->code == INPUT_ABS_Z) {
        d->prov_z = handelse->value;

        behandla_prov(d, k);

        handelse->type = INPUT_EV_KEY;
        handelse->code = INPUT_BTN_0;
        handelse->value = d->ut_knapp;
    }

    return 0;
}

static int paljett_init(const struct device *dev) {
    const struct paljett_konfig *k = dev->config;
    struct paljett_data *d = dev->data;

    memset(d, 0, sizeof(*d));

    LOG_DBG("paljett absolut: min %d max %d fart_max %d potens %d", k->min_faktor, k->max_faktor,
            k->fart_max, (int)KURV_POTENS);

    return 0;
}

static const struct zmk_input_processor_driver_api paljett_api = {
    .handle_event = paljett_hantera,
};

static struct paljett_data paljett_data_0;

static const struct paljett_konfig paljett_konfig_0 = {
    .min_faktor = DT_PROP(PALJETT_NOD, min_factor),
    .max_faktor = DT_PROP(PALJETT_NOD, max_factor),
    .fart_max = DT_PROP(PALJETT_NOD, speed_max),
};

DEVICE_DT_DEFINE(PALJETT_NOD, paljett_init, NULL, &paljett_data_0, &paljett_konfig_0, POST_KERNEL,
                 CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &paljett_api);
