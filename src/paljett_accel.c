#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <drivers/input_processor.h>

struct accel_config {
    int min_factor;
    int max_factor;
    int speed_max;
};

static int accel_handle(const struct device *dev, struct input_event *event,
                        uint32_t param1, uint32_t param2,
                        struct zmk_input_processor_state *state) {
    const struct accel_config *cfg = dev->config;

    if (event->type != INPUT_EV_REL) {
        return 0;
    }
    if (event->code != INPUT_REL_X && event->code != INPUT_REL_Y) {
        return 0;
    }

    int v = event->value;
    int mag = v < 0 ? -v : v;

    int factor;
    if (mag >= cfg->speed_max) {
        factor = cfg->max_factor;
    } else {
        /* kubisk kurva: halls nere lange, stiger brant pa slutet */
        int t = (mag * 1000) / cfg->speed_max;
        int t3 = (((t * t) / 1000) * t) / 1000;
        factor = cfg->min_factor +
                 ((cfg->max_factor - cfg->min_factor) * t3) / 1000;
    }

    event->value = (v * factor) / 1000;
    return 0;
}

static const struct zmk_input_processor_driver_api accel_api = {
    .handle_event = accel_handle,
};

static int accel_init(const struct device *dev) { return 0; }

static const struct accel_config accel_cfg_0 = {
    .min_factor = DT_PROP(DT_NODELABEL(paljett_accel), min_factor),
    .max_factor = DT_PROP(DT_NODELABEL(paljett_accel), max_factor),
    .speed_max = DT_PROP(DT_NODELABEL(paljett_accel), speed_max),
};

DEVICE_DT_DEFINE(DT_NODELABEL(paljett_accel), accel_init, NULL, NULL,
                 &accel_cfg_0, POST_KERNEL,
                 CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &accel_api);
