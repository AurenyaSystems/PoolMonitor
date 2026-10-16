# PoolMonitor

ESP32 firmware for a solar-powered pool monitor. It measures water chemistry and temperature, watches the electronics enclosure, and publishes raw readings over MQTT to [pool-monitor-web](https://github.com/AurenyaSystems/pool-monitor-web), which stores them and does all derived maths (dew point, LSI, drift).

Planning lives in Jira, project `POOL`. Ticket numbers in commits and branches refer to it.

## What it measures (v1)

| Reading | Sensor | ID | Unit |
|---|---|---|---|
| pH | Atlas Scientific EZO-pH on an ISCCB-2 isolated carrier, I2C | `PH` | pH |
| ORP | Atlas Scientific EZO-ORP on an ISCCB-2 isolated carrier, I2C | `ORP` | mV |
| Water temperature | DS18B20 waterproof probe, 1-Wire | `WATER_T` | °C |
| Enclosure temperature | DHT22 | `ENC_T` | °C |
| Enclosure humidity | DHT22 | `ENC_RH` | %RH |

The water temperature is also passed to the pH stamp for temperature compensation. The stamp does the maths.

Free chlorine by colorimetry is post-v1.

## Wiring (generic ESP32 devkit, provisional)

| Signal | GPIO | Notes |
|---|---|---|
| DHT22 data | 4 | external pull-up on the module |
| DS18B20 data | 27 | **4.7 kΩ pull-up to 3.3 V**; external power, not parasitic |
| I2C SDA | 21 | EZO-pH at `0x63`, EZO-ORP at `0x62` |
| I2C SCL | 22 | 100 kHz |

EZO stamps ship in UART mode. Switch them to I2C before connecting them.

## Firmware layout

```
core/      bus code: bit-banged 1-Wire
drivers/   one class per device (DHT, DS18B20, EZO, WiFi, MQTT) plus time sync
           and the persistent seq counter
app/       telemetry: sampler tasks -> reading queue -> transmitter (no ESP-IDF calls)
main/      app_main: brings everything up and wires it together
```

- **Samplers:** each sensor has its own task and interval (30 min by default). A reading is only queued if the sample succeeded and the clock has been set by SNTP.
- **Transmitter:** `app_main`'s loop drains the queue to MQTT every 60 s.
- **Offline:** while the broker is unreachable, readings stay queued (up to 64; the oldest are dropped first).

Power: automatic light sleep plus WiFi modem sleep, configured in `sdkconfig.defaults`.

## MQTT contract (POOL-38)

| Topic | QoS | Retained | Payload |
|---|---|---|---|
| `pool/<device_id>/telemetry` | 1 | no | `{"schema":1,"seq":N,"id":"PH","ts":<epoch>,"v":7.42}` |
| `pool/<device_id>/status` | 1 | yes | `online` / `offline` (LWT) |

- **One message per reading.**
- **Units:** SI, except ORP in mV. No units in the payload; the server maps IDs to units.
- **`seq`:** persisted in NVS and never reused across reboots.
- **`ts`:** when the reading was taken.
- **`device_id`:** the factory MAC as 12 hex digits, unless `MQTT_DEVICE_ID` is set in `secrets.h`.

## Building

Uses ESP-IDF **v6.0.2**.

1. Copy `main/secrets.h.example` to `main/secrets.h` and fill in WiFi and broker details. It's gitignored. `SNTP_SERVER` and `MQTT_DEVICE_ID` are optional.
2. `idf.py build flash monitor`

> **Delete `sdkconfig` whenever `sdkconfig.defaults` changes.** The defaults only apply when `sdkconfig` is regenerated, and `idf.py fullclean` does not delete it. A stale `sdkconfig` without power management makes `esp_pm_configure()` fail and the board reboot-loop.

## Bring-up checklist

- **Boot log:**
  - the DS18B20 ROM ID (`probe on GPIO 27: ROM ...`)
  - each EZO info reply (`0x63: ?I,pH,...`)
  - `first sync: <UTC> at uptime <ms>`
- **Nothing is published before the first SNTP sync.** That's intentional.
- **Readings arrive at the broker** within about a minute of being sampled, with consecutive `seq`.
- **After a reboot,** `seq` continues instead of restarting.

## Status

Drivers for every v1 sensor, SNTP, the telemetry pipeline and WiFi backoff compile, but **most have not been validated on hardware yet**; see the POOL tickets in review. The DHT22 driver is hardware-validated.

Still open:
- How EZO calibrations are triggered and where calibration history is recorded (POOL-10)
- RTC for offline boot (POOL-2)
- Persistent buffering (POOL-3)
- OTA (POOL-5)
- Power characterisation (POOL-44)
