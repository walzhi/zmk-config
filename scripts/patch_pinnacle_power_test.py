#!/usr/bin/env python3
"""Inject a USB-only shutdown diagnostic into the upstream Pinnacle driver.

The injection is guarded by CONFIG_PALJETT_SENSOR_POWER_TEST; it does not
change normal firmware. Run after patch_pinnacle_driver.py. Fail closed if
upstream anchors change. SPI is slowed to 100 kHz in this diagnostic because
the Cirque interface note flags special wake handling at >= 1 MHz.
"""
from pathlib import Path
import sys

MARKER = '/* PALJETT_SENSOR_POWER_TEST_V1 */'

HEADERS = '''
#if defined(CONFIG_PALJETT_SENSOR_POWER_TEST)
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/bluetooth/conn.h>
#endif
'''

CODE = r'''
/* PALJETT_SENSOR_POWER_TEST_V1 */
#if defined(CONFIG_PALJETT_SENSOR_POWER_TEST)
static const struct device *paljett_test_dev;
static atomic_t paljett_test_irqs;
static uint32_t paljett_test_packets;
static uint32_t paljett_test_previous_packets;
static int64_t paljett_test_last_touch;
static int64_t paljett_test_last_log;
static bool paljett_test_shutdown_attempted;

static void paljett_test_bt_connected(struct bt_conn *conn, uint8_t err)
{
    ARG_UNUSED(conn);
    LOG_INF("PALJETT Bluetooth connection result: err=%u", err);
}

static void paljett_test_bt_disconnected(struct bt_conn *conn, uint8_t reason)
{
    ARG_UNUSED(conn);
    LOG_INF("PALJETT Bluetooth disconnected: reason=0x%02x", reason);
}

BT_CONN_CB_DEFINE(paljett_test_bt_callbacks) = {
    .connected = paljett_test_bt_connected,
    .disconnected = paljett_test_bt_disconnected,
};

static void paljett_test_monitor(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(paljett_test_monitor_work, paljett_test_monitor);

static void paljett_test_boot(void)
{
    uint32_t cause = 0;
    int rc = hwinfo_get_reset_cause(&cause);
    LOG_INF("PALJETT POWER TEST v1: boot; reset-cause rc=%d flags=0x%08x", rc, cause);
    if (rc == 0) {
        LOG_INF("PALJETT reset: pin=%d power=%d software=%d watchdog=%d low-power=%d",
                !!(cause & RESET_PIN), !!(cause & RESET_POR),
                !!(cause & RESET_SOFTWARE), !!(cause & RESET_WATCHDOG),
                !!(cause & RESET_LOW_POWER_WAKE));
        hwinfo_clear_reset_cause();
    }
    LOG_INF("PALJETT test: USB/BT stay awake; only sensor shuts down after 30s idle");
    LOG_INF("PALJETT test: SPI=100kHz; IRQ and packet handling stay enabled");
}

static void paljett_test_start(const struct device *dev)
{
    paljett_test_dev = dev;
    paljett_test_last_touch = k_uptime_get();
    paljett_test_last_log = paljett_test_last_touch;
    LOG_INF("PALJETT sensor initialization complete; use trackpad, then leave it untouched");
    k_work_schedule(&paljett_test_monitor_work, K_SECONDS(1));
}

static void paljett_test_monitor(struct k_work *work)
{
    ARG_UNUSED(work);
    const struct pinnacle_config *config = paljett_test_dev->config;
    int64_t now = k_uptime_get();
    int rc;
    uint8_t value = 0;

    if (!paljett_test_shutdown_attempted &&
        now - paljett_test_last_touch >= 30000) {
        paljett_test_shutdown_attempted = true;
        LOG_INF("PALJETT shutdown requested: idle >=30s, packets=%u IRQs=%u",
                paljett_test_packets, (unsigned int)atomic_get(&paljett_test_irqs));
        rc = pinnacle_read(paljett_test_dev, PINNACLE_REG_SYS_CONFIG1, &value);
        LOG_INF("PALJETT before shutdown: read rc=%d SysConfig1=0x%02x", rc, value);
        if (rc == 0) {
            value |= PINNACLE_SYS_CONFIG1_SHUTDOWN;
            rc = pinnacle_write(paljett_test_dev, PINNACLE_REG_SYS_CONFIG1, value);
            LOG_INF("PALJETT shutdown write: SPI transfer rc=%d value=0x%02x", rc, value);
            if (rc == 0) {
                k_msleep(2);
                value = 0;
                rc = pinnacle_read(paljett_test_dev, PINNACLE_REG_SYS_CONFIG1, &value);
                if (rc == 0 && (value & PINNACLE_SYS_CONFIG1_SHUTDOWN)) {
                    LOG_INF("PALJETT shutdown bit VERIFIED: SysConfig1=0x%02x", value);
                } else {
                    LOG_ERR("PALJETT shutdown NOT verified: read rc=%d value=0x%02x", rc, value);
                }
            }
        }
        paljett_test_previous_packets = paljett_test_packets;
        paljett_test_last_log = now;
        LOG_INF("PALJETT try touching pad now; press reset ONCE to restart");
    }

    if (now - paljett_test_last_log >= 5000) {
        uint32_t delta = paljett_test_packets - paljett_test_previous_packets;
        value = 0;
        rc = pinnacle_read(paljett_test_dev, PINNACLE_REG_SYS_CONFIG1, &value);
        int dr = gpio_pin_get_dt(&config->dr_gpio);
        LOG_INF("PALJETT monitor: phase=%s idle_ms=%u read_rc=%d config=0x%02x shutdown=%d DR=%d IRQs=%u packets=%u delta=%u",
                paljett_test_shutdown_attempted ? "after-command" : "active",
                (unsigned int)(now - paljett_test_last_touch), rc, value,
                rc == 0 ? !!(value & PINNACLE_SYS_CONFIG1_SHUTDOWN) : -1,
                dr, (unsigned int)atomic_get(&paljett_test_irqs),
                paljett_test_packets, delta);
        if (paljett_test_shutdown_attempted && delta != 0) {
            LOG_WRN("PALJETT packets arrived after shutdown attempt; inspect touch data (first interval may contain a pending packet)");
        }
        paljett_test_previous_packets = paljett_test_packets;
        paljett_test_last_log = now;
    }
    k_work_schedule(&paljett_test_monitor_work, K_SECONDS(1));
}
#endif
'''

PACKETS = '''
#if defined(CONFIG_PALJETT_SENSOR_POWER_TEST)
    paljett_test_packets++;
    if (config->relative_mode) {
        LOG_DBG("PALJETT sample: dx=%d dy=%d", sample->rel_x, sample->rel_y);
    } else {
        LOG_DBG("PALJETT sample: x=%u y=%u z=%u", sample->abs_x, sample->abs_y, sample->abs_z);
    }
    if ((config->relative_mode && (sample->rel_x || sample->rel_y)) ||
        (!config->relative_mode && !pinnacle_is_idle_sample(sample))) {
        paljett_test_last_touch = k_uptime_get();
    }
    if (paljett_test_shutdown_attempted) {
        LOG_WRN("PALJETT sensor sample AFTER shutdown attempt");
    }
#endif
'''


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise RuntimeError(f'Expected exactly one upstream anchor: {old[:85]!r}')
    return source.replace(old, new, 1)


def patch_driver(path):
    source = path.read_text()
    if MARKER in source:
        return 'Paljett sensor power test is already present.'
    source = replace_once(source, '#include <zephyr/sys/util.h>',
                          '#include <zephyr/sys/util.h>' + HEADERS)
    source = replace_once(source, 'static int pinnacle_handle_interrupt(const struct device *dev)',
                          CODE + '\nstatic int pinnacle_handle_interrupt(const struct device *dev)')
    source = replace_once(source, '\tif (config->relative_mode) {\n\t\tinput_report_rel',
                          PACKETS + '\n\tif (config->relative_mode) {\n\t\tinput_report_rel')
    source = replace_once(source, '\tk_work_submit(&drv_data->work);',
                          '#if defined(CONFIG_PALJETT_SENSOR_POWER_TEST)\n'
                          '\tatomic_inc(&paljett_test_irqs);\n#endif\n'
                          '\tk_work_submit(&drv_data->work);')
    source = replace_once(source, 'static int pinnacle_init(const struct device *dev)\n{',
                          'static int pinnacle_init(const struct device *dev)\n{\n'
                          '#if defined(CONFIG_PALJETT_SENSOR_POWER_TEST)\n'
                          '\tpaljett_test_boot();\n#endif')
    source = replace_once(source, '\n\treturn 0;\n}\n\n#define PINNACLE_CONFIG_BUS_I2C',
                          '\n#if defined(CONFIG_PALJETT_SENSOR_POWER_TEST)\n'
                          '\tpaljett_test_start(dev);\n#endif\n'
                          '\treturn 0;\n}\n\n#define PINNACLE_CONFIG_BUS_I2C')
    for func in ('pinnacle_write_spi', 'pinnacle_seq_write_spi',
                 'pinnacle_read_spi', 'pinnacle_seq_read_spi'):
        start = source.index('static int ' + func + '(')
        opening = source.index('{', start)
        ending = source.index('\n}', opening)
        body = source[opening:ending]
        if '&bus->spi,' not in body:
            raise RuntimeError(f'No SPI transfer found in {func}')
        body = body.replace('&bus->spi,', '&paljett_spi,')
        body = body.replace('{', '''{
    struct spi_dt_spec paljett_spi = bus->spi;
#if defined(CONFIG_PALJETT_SENSOR_POWER_TEST)
    paljett_spi.config.frequency = 100000;
#endif
''', 1)
        source = source[:opening] + body + source[ending:]
    path.write_text(source)
    return 'Applied Paljett USB sensor shutdown diagnostic v1.'


def main():
    if len(sys.argv) != 2:
        print('usage: patch_pinnacle_power_test.py /path/to/input_pinnacle.c', file=sys.stderr)
        return 2
    try:
        print(patch_driver(Path(sys.argv[1])))
    except (OSError, RuntimeError, ValueError) as exc:
        print(f'Paljett power-test patch failed: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
