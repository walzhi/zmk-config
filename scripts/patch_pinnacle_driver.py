#!/usr/bin/env python3
"""Add bounded startup recovery to Zephyr's Cirque Pinnacle input driver."""

from pathlib import Path
import sys


MARKER = "/* PALJETT_INIT_RECOVERY */"

RECOVERY = '''    /* PALJETT_INIT_RECOVERY */
    bool firmware_id_valid = false;
    int firmware_id_rc = 0;

    /* A transient bad read can leave the sensor unusable until reset. */
    for (int attempt = 0; attempt < 3; ++attempt) {
        firmware_id_rc = pinnacle_read(dev, PINNACLE_REG_FIRMWARE_ID, &value);
        if (firmware_id_rc == 0 && value == PINNACLE_FIRMWARE_ID) {
            firmware_id_valid = true;
            break;
        }

        if (attempt == 2) {
            break;
        }

        if (firmware_id_rc != 0) {
            LOG_WRN("Failed to read FirmwareId (%d); resetting sensor and retrying", firmware_id_rc);
        } else {
            LOG_WRN("Unexpected Firmware ASIC ID %x; resetting sensor and retrying", value);
        }

        rc = pinnacle_write(dev, PINNACLE_REG_SYS_CONFIG1, PINNACLE_SYS_CONFIG1_RESET);
        if (rc < 0) {
            LOG_ERR("Failed to reset Pinnacle sensor during recovery (%d)", rc);
            return rc;
        }

        /* The sensor needs time to restart before another register access. */
        k_msleep(30);
    }

    if (!firmware_id_valid) {
        if (firmware_id_rc != 0) {
            LOG_ERR("Failed to read FirmwareId after recovery attempts (%d)", firmware_id_rc);
            return firmware_id_rc;
        }

        LOG_ERR("Incorrect Firmware ASIC ID %x after recovery attempts", value);
        return -ENODEV;
    }

'''


def patch_driver(path: Path) -> str:
    if not path.is_file():
        raise RuntimeError(f"Zephyr Pinnacle driver not found: {path}")

    source = path.read_text(encoding="utf-8")
    if MARKER in source:
        return "Paljett Cirque startup recovery is already present."

    init_start = source.find("static int pinnacle_init(const struct device *dev)")
    if init_start < 0:
        raise RuntimeError("Could not find pinnacle_init() in the Zephyr driver.")

    bus_check_end = source.find("/* Clear CC */", init_start)
    if bus_check_end < 0:
        raise RuntimeError("Could not find the expected initialization sequence in the Zephyr driver.")

    prefix = source[init_start:bus_check_end]
    start = prefix.find("rc = pinnacle_read(dev, PINNACLE_REG_FIRMWARE_ID, &value);")
    if start < 0:
        raise RuntimeError("Could not find the FirmwareId check in the Zephyr driver.")

    # Replace only the FirmwareId read/check block; retain the bus readiness check.
    prefix = prefix[:start] + RECOVERY
    patched = source[:init_start] + prefix + source[bus_check_end:]

    if patched == source:
        raise RuntimeError("The Zephyr driver was not modified.")

    path.write_text(patched, encoding="utf-8")
    return "Applied Paljett Cirque startup recovery (up to two sensor resets)."


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: patch_pinnacle_driver.py /path/to/input_pinnacle.c", file=sys.stderr)
        return 2

    try:
        print(patch_driver(Path(sys.argv[1])))
    except (OSError, RuntimeError) as exc:
        print(f"Pinnacle recovery patch failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
