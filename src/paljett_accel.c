#define DT_DRV_COMPAT zmk_input_processor_paljett_accel

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

    int factor = cfg->min_factor +
                 ((cfg->max_factor - cfg->min_factor) * mag * mag) /
                 (cfg->speed_max * cfg->speed_max);
    if (factor > cfg->max_factor) {
        factor = cfg->max_factor;
    }

    event->value = (v * factor) / 1000;
    return 0;
}

static const struct zmk_input_processor_driver_api accel_api = {
    .handle_event = accel_handle,
};

static int accel_init(const struct device *dev) { return 0; }

#define ACCEL_INST(n)                                                          \
    static const struct accel_config accel_cfg_##n = {                         \
        .min_factor = DT_INST_PROP(n, min_factor),                             \
        .max_factor = DT_INST_PROP(n, max_factor),                             \
        .speed_max = DT_INST_PROP(n, speed_max),                               \
    };                                                                         \
    DEVICE_DT_INST_DEFINE(n, accel_init, NULL, NULL, &accel_cfg_##n,           \
                          POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,    \
                          &accel_api);

DT_INST_FOREACH_STATUS_OKAY(ACCEL_INST)
