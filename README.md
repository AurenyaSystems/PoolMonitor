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
| `pool/<device_id>/cmd` | 1 | no | one console command as plain text, e.g. `ph cal mid 7.00` (provisional) |
| `pool/<device_id>/cmd/response` | 1 | no | the command's reply, plain text |
| `pool/<device_id>/sysinfo/<leaf>` | 1 | yes | latest device state, one leaf per item (below) |

`sysinfo` leaves published today:

| Leaf | When | Payload |
|---|---|---|
| `version` | at boot | the firmware version string (`git describe`) |
| `sensor_status/<ID>` | when a sensor's status changes | `{"status":"DEGRADED","consecutive":3,"severity":"warn"}` with severity `ok` / `warn` / `error` |
| `calibration/<ID>` | after a successful `cal` command | `{"ts":1760640000,"cmd":"mid,7.00","points":1}`; `ts` is omitted if the clock isn't synced |

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

## Console and server commands

The same commands work from the UART console (`idf.py monitor`, prompt `pool>`) and from the MQTT `cmd` topic. Type `help` for the list.

| Command | What it does |
|---|---|
| `status` | one line per subsystem: WiFi, MQTT, time, seq, queue, every sensor with its last value and age |
| `dht read` / `dht status` | read the enclosure sensor now, or show its last reading |
| `water read` / `water status` / `water rom` | read the DS18B20 now, show its last reading, or print its ROM ID |
| `ph read` / `ph status` | read the pH stamp now, or show its last reading |
| `ph temp <degC>` | send a temperature for compensation (the sampler does this automatically) |
| `ph cal mid 7.00`, `ph cal low 4.00`, `ph cal high 10.00` | calibrate; `mid` first, it clears the others |
| `ph cal clear` / `ph cal ?` | clear calibration, or ask how many points are stored |
| `orp read` / `orp status` / `orp cal 225` / `orp cal clear` / `orp cal ?` | same for ORP, calibrated against the 225 mV standard |
| `wifi status` / `wifi ps on\|off` | link state and disconnect count; toggle modem sleep |
| `mqtt status` | connection state |
| `queue status` / `queue push <id> <value>` | readings waiting; push a reading through the real pipeline (**the server stores it**) |
| `time` / `seq` | clock and sync state; next sequence number |
| `reboot` | restart in 1 s, after the reply is sent |

A read from the console waits for the sampler if one is mid-read, so it can take up to a second.

With light sleep enabled, the first keystrokes after a quiet period may be lost. Press Enter once, then type.

## Bring-up checklist

- **Boot log:**
  - the DS18B20 ROM ID (`probe on GPIO 27: ROM ...`)
  - each EZO info reply (`0x63: ?I,pH,...`)
  - `first sync: <UTC> at uptime <ms>`
- **Nothing is published before the first SNTP sync.** That's intentional.
- **Readings arrive at the broker** within about a minute of being sampled, with consecutive `seq`.
- **After a reboot,** `seq` continues instead of restarting.

## Host tests

`test/host/run.sh` compiles the real driver and app sources against stub ESP-IDF headers and drives them with simulated hardware: a bit-level 1-Wire bus with DS18B20s on it, a fake I2C EZO stamp, and a recording MQTT client. It needs only `g++`, takes a few seconds, and has caught real bugs before they reached the board. Run it before opening a PR.

It can't check electrical timing, pull-ups or anything in ESP-IDF itself. Those still need the board.

## Status

Drivers for every v1 sensor, SNTP, the telemetry pipeline and WiFi backoff compile, but **most have not been validated on hardware yet**; see the POOL tickets in review. The DHT22 driver is hardware-validated.

Still open:
- How EZO calibrations are triggered and where calibration history is recorded (POOL-10)
- RTC for offline boot (POOL-2)
- Persistent buffering (POOL-3)
- OTA (POOL-5)
- Power characterisation (POOL-44)
