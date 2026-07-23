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
`brother/controller/transports.py` in the `jimparis/labels` repository.

