# ESPHome P-touch Bridge

This project turns an original ESP32 into a Wi-Fi to Bluetooth Classic bridge
for Brother P-touch printers. It was developed for the PT-P710BT and the label
editor in [jimparis/labels](https://github.com/jimparis/labels).

The ESP32 maintains the short-range Bluetooth SPP connection near the printer.
The label server sends compact, already-rasterized Brother print commands to the
bridge over an authenticated HTTP API. ESPHome supplies Wi-Fi provisioning,
Home Assistant integration, diagnostics, OTA updates, and dashboard adoption.

## Installation

Install the pre-built firmware from:

https://jimparis.github.io/esphome-ptouch-bridge/

Or clone this repository and run:

```sh
uv sync
make build
make flash
```

The firmware targets an original ESP32 (`esp32dev`) because the printer uses
Bluetooth Classic SPP. ESP8266 and BLE-only ESP32 variants are not compatible.

## ESPHome Device Builder configuration

After adopting the device, keep private values in your deployed YAML:

```yaml
substitutions:
  printer_address: "BC:31:98:A0:63:5F"
  bridge_http_token: "replace-with-a-long-random-token"

packages:
  ptouch_bridge:
    url: https://github.com/jimparis/esphome-ptouch-bridge
    files:
      - packages/ptouch_bridge.yaml
    ref: main

external_components:
  - source: github://jimparis/esphome-ptouch-bridge@main
    components:
      - ptouch_bridge

api:
  encryption:
    key: "YOUR-DEVICE-SPECIFIC-API-KEY"

ota:
  - platform: esphome
    password: !secret ota_password
```

The bridge token is used by the label server's HTTP requests. The ESPHome API
key may also be inline in this private deployed config. Wi-Fi credentials and
the shared OTA password remain in the Device Builder's `secrets.yaml`.

## Diagnostics and persistence

Home Assistant receives printer connection state, current and last detected
cartridge, last print result, Bluetooth attempt/connection counts, confirmed
label count, and estimated tape usage. Counters use `total_increasing` state.

These counters deliberately reset whenever the ESP32 boots; the component never
writes them to ESP32 NVS. Home Assistant should receive each confirmed print
before shutdown and retain long-term statistics across those resets.

Tape usage is calculated from the raster length plus the configured Brother
feed margins. It is an estimate of tape advanced for completed labels, not a
measurement of the remaining cassette.

## HTTP API

All routes accept `Authorization: Bearer TOKEN` when `http_token` is nonempty:

- `GET /api/v1/bridge/info`
- `POST /api/v1/bridge/status`
- `POST /api/v1/bridge/page`
- `POST /api/v1/bridge/disconnect`

The page route accepts a Brother raster command stream. The label server also
sends `X-Ptouch-Tape-Length-Dots`, allowing the bridge to publish tape usage
only after the printer reports completion.

