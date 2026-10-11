#!/usr/bin/env python3
"""Connect Pinnacle shutdown to ZMK's device-suspend phase before deep sleep.

Run after patch_pinnacle_driver.py. The sensor's SPI bus must remain active
until this device callback finishes (normal reverse device suspend order).
Use 125 kHz in the shield overlay, as exercised by the USB diagnostic.
"""
from pathlib import Path
import sys

MARKER = '/* PALJETT_PINNACLE_SLEEP_V1 */'
HEADERS = '''
#include <zephyr/pm/device.h>
#include <zephyr/sys/atomic.h>
'''
FIELDS = '''
#if defined(CONFIG_PALJETT_SENSOR_SLEEP)
    struct k_mutex sleep_lock;
    atomic_t sleeping;
    bool shutdown_sent;
#endif
'''
WORK = '''
#if defined(CONFIG_PALJETT_SENSOR_SLEEP)
    /* Serialize packet transfers with the final shutdown write. IRQ callbacks
     * may race with cancellation, so queued work must also check sleeping. */
    k_mutex_lock(&drv_data->sleep_lock, K_FOREVER);
    if (!atomic_get(&drv_data->sleeping)) {
        pinnacle_handle_interrupt(drv_data->dev);
    }
    k_mutex_unlock(&drv_data->sleep_lock);
#else
    pinnacle_handle_interrupt(drv_data->dev);
#endif
'''
CODE = r'''
/* PALJETT_PINNACLE_SLEEP_V1 */
#if defined(CONFIG_PALJETT_SENSOR_SLEEP)
/* Called only when deep sleep is aborted, while the MCU remains running.
 * Reset after actual power-off runs pinnacle_init() instead. Lock is held. */
static int pinnacle_sleep_restore_locked(const struct device *dev)
{
    const struct pinnacle_config *config = dev->config;
    struct pinnacle_data *drv_data = dev->data;
    uint8_t value = config->sleep_mode_enable ? PINNACLE_SYS_CONFIG1_LOW_POWER_MODE : 0;
    int rc = pinnacle_write(dev, PINNACLE_REG_SYS_CONFIG1, value);
    if (rc != 0) {
        LOG_ERR("PALJETT failed to restore sensor after aborted sleep (%d)", rc);
        return rc;
    }
    k_msleep(30);
    rc = gpio_pin_interrupt_configure_dt(&config->dr_gpio, GPIO_INT_EDGE_TO_ACTIVE);
    if (rc != 0) {
        LOG_ERR("PALJETT failed to restore sensor IRQ (%d)", rc);
        return rc;
    }
    atomic_clear(&drv_data->sleeping);
    drv_data->shutdown_sent = false;
    /* A data-ready edge may have occurred while interrupts were disabled. */
    if (gpio_pin_get_dt(&config->dr_gpio) > 0) {
        k_work_submit(&drv_data->work);
    }
    LOG_INF("PALJETT sensor restored after aborted deep sleep");
    return 0;
}

static int pinnacle_pm_action(const struct device *dev, enum pm_device_action action)
{
    const struct pinnacle_config *config = dev->config;
    struct pinnacle_data *drv_data = dev->data;
    uint8_t value;
    int rc = 0;

    switch (action) {
    case PM_DEVICE_ACTION_SUSPEND:
        k_mutex_lock(&drv_data->sleep_lock, K_FOREVER);
        if (atomic_get(&drv_data->sleeping) && drv_data->shutdown_sent) {
            k_mutex_unlock(&drv_data->sleep_lock);
            return 0;
        }
        rc = gpio_pin_interrupt_configure_dt(&config->dr_gpio, GPIO_INT_DISABLE);
        if (rc != 0) {
            k_mutex_unlock(&drv_data->sleep_lock);
            return rc;
        }
        atomic_set(&drv_data->sleeping, 1);
        drv_data->shutdown_sent = false;
        /* Do not synchronously wait for work on the current system queue. */
        (void)k_work_cancel(&drv_data->work);
        value = PINNACLE_SYS_CONFIG1_SHUTDOWN;
        if (config->sleep_mode_enable) {
            value |= PINNACLE_SYS_CONFIG1_LOW_POWER_MODE;
        }
        LOG_INF("PALJETT deep sleep: shutting down sensor first");
        rc = pinnacle_write(dev, PINNACLE_REG_SYS_CONFIG1, value);
        if (rc != 0) {
            LOG_ERR("PALJETT shutdown transfer failed (%d); aborting deep sleep", rc);
            /* ZMK does not resume a device whose suspend callback failed. */
            (void)pinnacle_sleep_restore_locked(dev);
            k_mutex_unlock(&drv_data->sleep_lock);
            return rc;
        }
        /* No register readback or packet transfer after this write. */
        drv_data->shutdown_sent = true;
        k_msleep(30);
        LOG_INF("PALJETT sensor shutdown sent; XIAO can enter deep sleep; reset to wake");
        k_mutex_unlock(&drv_data->sleep_lock);
        return 0;

    case PM_DEVICE_ACTION_RESUME:
        k_mutex_lock(&drv_data->sleep_lock, K_FOREVER);
        if (atomic_get(&drv_data->sleeping)) {
            rc = pinnacle_sleep_restore_locked(dev);
        }
        k_mutex_unlock(&drv_data->sleep_lock);
        return rc;

    default:
        return -ENOTSUP;
    }
}

#define PALJETT_PINNACLE_PM_DEFINE(inst) PM_DEVICE_DT_INST_DEFINE(inst, pinnacle_pm_action)
#define PALJETT_PINNACLE_PM_GET(inst) PM_DEVICE_DT_INST_GET(inst)
#else
#define PALJETT_PINNACLE_PM_DEFINE(inst)
#define PALJETT_PINNACLE_PM_GET(inst) NULL
#endif
'''


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise RuntimeError(f'Expected exactly one upstream anchor: {old[:85]!r}')
    return source.replace(old, new, 1)


def patch_driver(path):
    source = path.read_text(encoding='utf-8')
    if MARKER in source:
        return 'Paljett Pinnacle deep sleep support is already present.'
    source = replace_once(source, '#include <zephyr/sys/util.h>',
                          '#include <zephyr/sys/util.h>' + HEADERS)
    source = replace_once(source, '\tstruct k_work work;\n};',
                          '\tstruct k_work work;\n' + FIELDS + '};')
    source = replace_once(source, '\tpinnacle_handle_interrupt(drv_data->dev);', WORK)
    source = replace_once(source, '\tk_work_submit(&drv_data->work);',
                          '#if defined(CONFIG_PALJETT_SENSOR_SLEEP)\n'
                          '\tif (atomic_get(&drv_data->sleeping)) {\n'
                          '\t\treturn;\n\t}\n#endif\n'
                          '\tk_work_submit(&drv_data->work);')
    source = replace_once(source, 'static int pinnacle_init(const struct device *dev)\n{',
                          'static int pinnacle_init(const struct device *dev)\n{\n'
                          '#if defined(CONFIG_PALJETT_SENSOR_SLEEP)\n'
                          '\tstruct pinnacle_data *drv_data = dev->data;\n'
                          '\tk_mutex_init(&drv_data->sleep_lock);\n'
                          '\tatomic_clear(&drv_data->sleeping);\n#endif')
    source = replace_once(source, '#define PINNACLE_CONFIG_BUS_I2C(inst)',
                          CODE + '\n#define PINNACLE_CONFIG_BUS_I2C(inst)')
    source = replace_once(source,
                          '\tDEVICE_DT_INST_DEFINE(inst, pinnacle_init, NULL, &pinnacle_data_##inst,',
                          '\tPALJETT_PINNACLE_PM_DEFINE(inst);' + ' ' * 48 + '\\\n'
                          '\tDEVICE_DT_INST_DEFINE(inst, pinnacle_init, PALJETT_PINNACLE_PM_GET(inst), &pinnacle_data_##inst,')
    path.write_text(source, encoding='utf-8')
    return 'Applied Paljett sensor shutdown before XIAO deep sleep (30 ms lead).'


def main():
    if len(sys.argv) != 2:
        print('usage: patch_pinnacle_sleep.py /path/to/input_pinnacle.c', file=sys.stderr)
        return 2
    try:
        print(patch_driver(Path(sys.argv[1])))
    except (OSError, RuntimeError, ValueError) as exc:
        print(f'Paljett sleep patch failed: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
