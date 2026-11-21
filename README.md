# OSSM-Stroke on ESP32-SvelteKit

An OSSM (Open Source Sex Machine) stroke controller. The motion code is the
`OSSM-RackandPinion` StrokeEngine, the network and control backend is
ESP32-SvelteKit. Everything the original firmware did with a cable knob, an OLED
screen or the M5 ESP-NOW remote is driven from a web browser instead: REST API,
WebSocket telemetry, MQTT for Home Assistant.

**Status: seed implementation.** It builds for both ESP32 targets below. It has
not been run on real OSSM hardware yet, so homing, sensorless stall detection and
motion quality are untested.

## Layout

| Path | Content |
| --- | --- |
| `src/main.cpp` | owns the engine, applies browser commands to it, pushes telemetry |
| `src/StrokeStateService.*` | `/rest/strokeState` and WebSocket event `stroke` |
| `src/StrokeSettingsService.*` | `/rest/strokeSettings`, persisted to LittleFS |
| `src/OSSM_PinDEF.h` | all pin numbers |
| `lib/StrokeEngine` | motion engine, pattern generator, homing (upstream) |
| `lib/framework`, `lib/PsychicHttp` | ESP32-SvelteKit backend (upstream) |
| `interface/` | SvelteKit front end, control page at `routes/stroke/` |
| `scripts/` | PlatformIO pre-build scripts: front end build, cert bundle, images |

## Build

PlatformIO with the pioarduino platform (Arduino core 3.3.2 / ESP-IDF 5.5) and
Node.js for the front end. The front end build runs automatically.

```sh
pio run -e esp32dev              # OSSM reference board, ESP32, 4 MB flash
pio run -e esp32-s3-devkitc-1    # recommended: ESP32-S3, 8 MB flash
pio run -e esp32dev -t upload    # flash over USB
pio run -e esp32dev -t erase     # one-time wipe when coming from the EEPROM firmware
```

### Update over the network

This firmware contains no espota/ArduinoOTA, so an update goes through the
device itself, the same way the browser does it on the System -> Update page:

```sh
scripts/ota-upload.py --device <ip> build/release/OSSM-Stroke_esp32dev_0-7-0.bin
scripts/ota-upload.py --device <ip> --check     # login and report the running build only
```

The device writes the image to the next OTA partition and reboots. Upload
progress is printed from the `otastatus` WebSocket event.

Flash usage measured for this seed: `esp32dev` 96.6 % of a 1,875 KiB app slot
(66 kB head room), `esp32-s3-devkitc-1` 55.7 % of a 3.19 MiB app slot. An 8 MB
module is the recommended target.

Serial output names the firmware after start-up. The web UI is served by the
device itself, default credentials `admin` / `admin` — change them before the
device is reachable from an untrusted network.

## Where the machine values live

Machine geometry, motor parameters, homing mode and the user speed limit are
build-time defaults in `platformio.ini` and runtime values in
`/rest/strokeSettings`, persisted as `/config/strokeSettings.json` on LittleFS.
The EEPROM block of the original firmware is gone.

## Credits and licences

This project contains code from two upstream projects. Their licences apply to
their parts:

| Upstream | Part used | Licence |
| --- | --- | --- |
| [ortlof/OSSM-Stroke](https://github.com/ortlof/OSSM-Stroke), branch `OSSM-RackandPinion` | StrokeEngine fork, OSSM pinout and machine values | Attribution-ShareAlike 4.0 International — see `LICENCE-OSSM-Stroke.txt` |
| [theelims/ESP32-sveltekit](https://github.com/theelims/ESP32-sveltekit) | back end framework, build chain, front end shell | LGPL-3.0 (back end), MIT (front end) — see `lib/framework/LICENSE` |
| [theelims/StrokeEngine](https://github.com/theelims/StrokeEngine) | motion engine base | MIT — see `lib/StrokeEngine/LICENSE` |

Vendored libraries keep their own licence files in `lib/`.
