/*
 * Paljett - pekaracceleration for ZMK
 *
 * Accelerationen raknas pa hela rorelsevektorns langd i stallet for pa
 * varje axel for sig, och avrundningsresten delas sa att bada axlarna
 * slapps ut samtidigt.
 *
 * Plattan rapporterar tappen som INPUT_BTN_TOUCH medan ZMK vantar sig
 * INPUT_BTN_0 for vansterklick, sa den koden skrivs om har.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <drivers/input_processor.h>

LOG_MODULE_REGISTER(paljett_accel, CONFIG_ZMK_LOG_LEVEL);

#define PALJETT_NOD DT_NODELABEL(paljett_accel)

/* Faktorn ar i tusendelar. min-factor 120 betyder 0,120. */
#define ENHET 1000

/* Kurvans potens. 1 rak linje, 2 mjuk boj, 3 brant. */
#define KURV_POTENS DT_PROP_OR(PALJETT_NOD, curve_power, 3)

/* Hur lang den samlade vektorn maste vara innan nagot slapps ut. */
#define TROSKEL DT_PROP_OR(PALJETT_NOD, release_threshold, ENHET)

/* Hogsta tid en rorelse far hallas kvar innan den slapps ut anda. */
#define HALL_MAX_US DT_PROP_OR(PALJETT_NOD, hold_max_us, 24000)

/* Reserv om sync-flaggan skulle utebli. */
#define PROV_US DT_PROP_OR(PALJETT_NOD, sample_us, 3000)

/* Stillhet sa har lange raknas som att fingret lyfts. */
#define VILA_US DT_PROP_OR(PALJETT_NOD, idle_us, 200000)

/* Golv och tak pa tiden mellan tva avlasningar. */
#define DT_GOLV_US 4000
#define DT_TAK_US 60000

/* Utjamning av farten, en gang per avlasning. */
#define UTJAMNING 16

/* Sakerhetsventil mot enstaka feltolkade prov. */
#define UT_TAK 2047

struct paljett_konfig {
    int32_t min_faktor;
    int32_t max_faktor;
    int32_t fart_max;
};

struct paljett_data {
    int32_t fart;

    int32_t prov_x;
    int32_t prov_y;

    int32_t ack_x;
    int32_t ack_y;

    int32_t ut_x;
    int32_t ut_y;

    int64_t senaste_us;
    int64_t prov_us;
    int64_t hall_us;

    bool grupp_oppen;
    bool ny_gest;

    int32_t knapp_forra;
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

static void nollstall(struct paljett_data *d, int64_t nu) {
    d->fart = 0;
    d->prov_x = 0;
    d->prov_y = 0;
    d->ack_x = 0;
    d->ack_y = 0;
    d->ut_x = 0;
    d->ut_y = 0;
    d->prov_us = nu;
    d->hall_us = nu;
    d->grupp_oppen = false;
    d->ny_gest = true;
}

static void stang_grupp(struct paljett_data *d, const struct paljett_konfig *k, int64_t nu) {
    int32_t rx = d->prov_x;
    int32_t ry = d->prov_y;

    d->prov_x = 0;
    d->prov_y = 0;
    d->grupp_oppen = false;

    int64_t dt_us = nu - d->prov_us;
    d->prov_us = nu;

    if (d->ny_gest) {
        d->ny_gest = false;
    } else {
        dt_us = CLAMP(dt_us, DT_GOLV_US, DT_TAK_US);

        int32_t momentan = (int32_t)(((int64_t)vektorlangd(rx, ry) * 1000000) / dt_us);

        momentan = MIN(momentan, k->fart_max * 2);

        d->fart = (d->fart * (UTJAMNING - 1) + momentan) / UTJAMNING;
    }

    int32_t faktor = kurva(k, d->fart);

    d->ack_x += rx * faktor;
    d->ack_y += ry * faktor;

    bool nog_lang = ((int64_t)d->ack_x * d->ack_x + (int64_t)d->ack_y * d->ack_y) >=
                    ((int64_t)TROSKEL * TROSKEL);
    bool tiden_ute = (nu - d->hall_us) >= HALL_MAX_US;

    if (!nog_lang && !tiden_ute) {
        return;
    }

    int32_t ux = avrunda(d->ack_x);
    int32_t uy = avrunda(d->ack_y);

    d->ack_x -= ux * ENHET;
    d->ack_y -= uy * ENHET;

    d->ut_x += ux;
    d->ut_y += uy;
    d->hall_us = nu;
}

static int paljett_hantera(const struct device *dev, struct input_event *handelse, uint32_t param1,
                           uint32_t param2, struct zmk_input_processor_state *tillstand) {
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    ARG_UNUSED(tillstand);

    struct paljett_data *d = dev->data;
    const struct paljett_konfig *k = dev->config;

    if (handelse->type == INPUT_EV_KEY) {
        if (handelse->code == INPUT_BTN_TOUCH) {
            handelse->code = INPUT_BTN_0;
        }

        if (handelse->value != d->knapp_forra) {
            d->knapp_forra = handelse->value;
            LOG_DBG("knapp: kod %d varde %d", handelse->code, handelse->value);
        }
    }

    bool rorelse = (handelse->type == INPUT_EV_REL) &&
                   (handelse->code == INPUT_REL_X || handelse->code == INPUT_REL_Y);

    int64_t nu = k_ticks_to_us_floor64(k_uptime_ticks());

    if (rorelse) {
        int64_t sedan = nu - d->senaste_us;
        d->senaste_us = nu;

        if (sedan > VILA_US) {
            nollstall(d, nu);
        } else if (d->grupp_oppen && sedan > PROV_US) {
            stang_grupp(d, k, nu);
        }

        if (handelse->code == INPUT_REL_X) {
            d->prov_x = handelse->value;
            handelse->value = CLAMP(d->ut_x, -UT_TAK, UT_TAK);
            d->ut_x -= handelse->value;
        } else {
            d->prov_y = handelse->value;
            handelse->value = CLAMP(d->ut_y, -UT_TAK, UT_TAK);
            d->ut_y -= handelse->value;
        }

        d->grupp_oppen = true;
    }

    if (handelse->sync && d->grupp_oppen) {
        stang_grupp(d, k, nu);
    }

    return 0;
}

static int paljett_init(const struct device *dev) {
    const struct paljett_konfig *k = dev->config;
    struct paljett_data *d = dev->data;

    nollstall(d, k_ticks_to_us_floor64(k_uptime_ticks()));
    d->senaste_us = 0;
    d->knapp_forra = 0;

    LOG_DBG("paljett accel: min %d max %d fart_max %d potens %d troskel %d", k->min_faktor,
            k->max_faktor, k->fart_max, (int)KURV_POTENS, (int)TROSKEL);

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
