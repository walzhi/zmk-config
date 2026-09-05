#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <drivers/input_processor.h>

#define HIST 4

struct accel_config {
    int min_factor;
    int max_factor;
    int speed_max;
};

struct accel_data {
    int hist_x[HIST];
    int hist_y[HIST];
    int idx;
    int rest_x;
    int rest_y;
    int64_t forra_tid;
    int fart;
};

static int accel_handle(const struct device *dev, struct input_event *event,
                        uint32_t param1, uint32_t param2,
                        struct zmk_input_processor_state *state) {
    const struct accel_config *cfg = dev->config;
    struct accel_data *d = dev->data;

    if (event->type != INPUT_EV_REL) {
        return 0;
    }
    if (event->code != INPUT_REL_X && event->code != INPUT_REL_Y) {
        return 0;
    }

    if (event->code == INPUT_REL_X) {
        d->idx = (d->idx + 1) % HIST;
        d->hist_x[d->idx] = event->value;
    } else {
        d->hist_y[d->idx] = event->value;
    }

    /* fart i steg per sekund, raknas for varje rapport oavsett axel */
    int64_t nu = k_uptime_get();
    int64_t dt = nu - d->forra_tid;
    d->forra_tid = nu;
    if (dt < 1) {
        dt = 1;
    }
    if (dt > 100) {
        dt = 100;
    }

    int stracka = 0;
    for (int i = 0; i < HIST; i++) {
        int x = d->hist_x[i], y = d->hist_y[i];
        stracka += (x < 0 ? -x : x) + (y < 0 ? -y : y);
    }
    int momentan = (stracka * 1000) / ((int)dt * HIST);

    d->fart = (d->fart * 3 + momentan) / 4;

    int factor;
    if (d->fart >= cfg->speed_max) {
        factor = cfg->max_factor;
    } else {
        int t = (d->fart * 1000) / cfg->speed_max;
        int t3 = (((t * t) / 1000) * t) / 1000;
        factor = cfg->min_factor +
                 ((cfg->max_factor - cfg->min_factor) * t3) / 1000;
    }

    const int *h = (event->code == INPUT_REL_X) ? d->hist_x : d->hist_y;
    int summa = 0;
    for (int i = 0; i < HIST; i++) {
        summa += h[i];
    }
    int jamnat = (event->value * 2 + summa) / (HIST + 2);

    int skalat = jamnat * factor;
    int *rest = (event->code == INPUT_REL_X) ? &d->rest_x : &d->rest_y;
    skalat += *rest;
    event->value = skalat / 1000;
    *rest = skalat - event->value * 1000;

    return 0;
}

static const struct zmk_input_processor_driver_api accel_api = {
    .handle_event = accel_handle,
};

static int accel_init(const struct device *dev) { return 0; }

static struct accel_data accel_data_0;

static const struct accel_config accel_cfg_0 = {
    .min_factor = DT_PROP(DT_NODELABEL(paljett_accel), min_factor),
    .max_factor = DT_PROP(DT_NODELABEL(paljett_accel), max_factor),
    .speed_max = DT_PROP(DT_NODELABEL(paljett_accel), speed_max),
};

DEVICE_DT_DEFINE(DT_NODELABEL(paljett_accel), accel_init, NULL,
                 &accel_data_0, &accel_cfg_0, POST_KERNEL,
                 CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &accel_api);
