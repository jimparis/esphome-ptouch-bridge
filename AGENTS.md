# Repository guide

This repository contains a public ESPHome external component and installable
firmware for an ESP32 Bluetooth Classic to Wi-Fi bridge for Brother P-touch
printers, initially the PT-P710BT.

## Development

- Pin ESPHome in `pyproject.toml` and manage Python dependencies with `uv`.
- Run `uv run esphome config ptouch_bridge.yaml` before compiling.
- Run `uv run esphome compile ptouch_bridge.yaml` for the distributable build.
- Do not add Wi-Fi credentials, API keys, HTTP tokens, or OTA passwords here.
- Runtime counters deliberately live only in RAM. Home Assistant is responsible
  for retaining and integrating their `total_increasing` state across reboots.
- Do not send a print job to a physical printer unless the user explicitly asks.

The HTTP API must remain compatible with
`controller/transports.py` in the `jimparis/labels-ptouch` repository.

## Live device builds and OTA

The ESPHome Device Builder on `fib` is the authoritative environment for the
installed bridge. Do not build or upload its private device configuration with a
host-side ESPHome installation, and do not invoke the Device Builder container
directly.

After pushing component changes, use the authenticated helper in the live
ESPHome configuration:

    ssh fib 'cd /opt/esphome && node tools/esphome-device-builder.js validate ptouch-bridge-2be0f4.yaml'
    ssh fib 'cd /opt/esphome && node tools/esphome-device-builder.js compile ptouch-bridge-2be0f4.yaml'
    ssh fib 'cd /opt/esphome && node tools/esphome-device-builder.js upload ptouch-bridge-2be0f4.yaml --device ptouch-bridge-2be0f4.home'

Validation and compilation may run while the printer is off. OTA requires the
printer—and therefore its embedded ESP32—to be powered on. The upload reboots
the ESP32; a separate physical power cycle is not normally needed.
