# PoolMonitor

Home project: an ESP32 pool monitor in C++ on ESP-IDF v6.0.2 (target `esp32`, generic devkit). It publishes over MQTT to a server in a separate repo (`pool-monitor-web`).

## Guiding principle: KISS

This is a home project, not a product. Keep it simple.

- Write the simplest code that works for the hardware actually on the bench. Don't plan for sensors, boards, or features we don't have.
- No design patterns or abstractions meant for large codebases (factories, registries, dependency injection, event buses, deep class hierarchies, template magic). Add a base class or interface only once a second real implementation exists.
- Use plain functions and small classes, and keep data flow direct. Choose readable code over clever code.
- Don't add speculative config options, generic frameworks, or "future-proofing".
- Keep changes small and scoped to the ticket, and don't refactor code you weren't asked to touch.
- If something can only be confirmed on hardware, say so. Don't guess.

## Planning lives in Jira

- The design decisions live in the Jira project `POOL` at aurenyasystems.atlassian.net, with one epic per subsystem. Read the relevant ticket before starting work. Don't re-decide what a ticket already settles.
- Branches are named `POOL-<n>-<short-slug>`. Commit messages start with `POOL-<n> `.
- Useful tickets:
  - POOL-9: DS18B20
  - POOL-10: EZO-pH
  - POOL-11: EZO-ORP
  - POOL-2 / POOL-37: time and RTC
  - POOL-36: telemetry publisher
  - POOL-38: MQTT topics and payload
  - POOL-13: bring-up and repo hygiene

## Coding style (match `drivers/` on main)

- Files are `snake_case`, with headers in `inc/` and sources in `src/`. Use `#ifndef FOO_H` include guards. Don't use `#pragma once`, except in `secrets.h`.
- Classes are `PascalCase` (`DhtSensor`, `MqttClient`), and methods and free functions are `snake_case` (`wait_for_connection`, `read_data`).
- Private members get a leading underscore (`_pin`, `_status`).
- A driver class usually has:
  - a constructor that only stores arguments, and a separate `init()`
  - `enum class ReturnCode` for results and `enum class Status { UNINITIALIZED, READY, DEGRADED, FAILED }` for health
  - a `get_status()` accessor
  - copy constructor and assignment deleted
- Constants are `constexpr static` class members or `constexpr` in an anonymous namespace in the `.cpp`. Names are `UPPER_SNAKE` or `kCamel`; follow the file you're in.
- Headers get Doxygen comments: `/** @brief ... @param ... @return ... */` on public methods and `///<` on members and enum values. Comments explain *why*, especially timing and hardware quirks.
- Function and class braces go on their own line. One-line guards stay inline: `if (!_events) { return ReturnCode::NOT_INITIALIZED; }`. Indent with 4 spaces.
- Logging uses `ESP_LOGx(TAG, ...)`, with `constexpr static const char* TAG = "[WIFI]";` in the class.
- Use fixed-size `char` buffers and `snprintf`, not `std::string`, in drivers.
- Timing-critical bit-banging runs inside `portENTER_CRITICAL` on a `portMUX_TYPE`, with the main task pinned to CPU1 (WiFi runs on CPU0).

## Layout

- `core/` holds bus code (`onewire`).
- `drivers/` holds sensor and peripheral drivers: `dht_sensor`, `ds18b20`, `ezo_sensor`, `wifi`, `mqtt`, `time_sync`, `seq_counter`.
- `app/` holds the telemetry pipeline (POOL-36): sampler tasks feed a `ReadingQueue`, and `run_transmitter` drains it to MQTT. It makes no direct ESP-IDF calls; FreeRTOS, POSIX and `ESP_LOGx` are fine.
- `main/main.cpp` sets up and wires everything. To add a sensor, write a driver, add a sampler in `app/`, and start its task in `main.cpp`.
- Every new `.cpp` goes into its component's `CMakeLists.txt`, along with any IDF component it uses (`REQUIRES` / `PRIV_REQUIRES`).
- Config that must survive a clean goes in `sdkconfig.defaults`. `sdkconfig` itself is gitignored. Defaults only apply when `sdkconfig` is regenerated, and `idf.py fullclean` does *not* delete it. After `sdkconfig.defaults` changes, delete `sdkconfig`. If you don't, `esp_pm_configure()` fails with `ESP_ERR_NOT_SUPPORTED` and boot aborts.
- Credentials live in the gitignored `main/secrets.h`, with `main/secrets.h.example` as the template.

## Data contract (POOL-38)

- Topics are `pool/<device_id>/telemetry` (QoS 1, not retained) and `pool/<device_id>/status` (retained `online`/`offline`, the LWT target).
- There is one flat message per reading: `{"schema":1,"seq":N,"id":"PH","ts":<epoch>,"v":7.42}`.
- Values are in SI units, with no units in the payload. Sensor IDs: `ENC_T`, `ENC_RH`, `WATER_T`, `PH`, `ORP`.
- Firmware publishes raw values only, with no derived values. Skip a reading when its sensor is DEGRADED or FAILED. Never publish null.

## Build

The ESP-IDF install is managed by EIM. Its activate script runs `eim select`, which opens the EIM GUI, so strip `eim_gui` from PATH first:

```sh
bash -c 'export PATH=$(echo "$PATH" | tr ":" "\n" | grep -v eim_gui | paste -sd:); \
  source ~/.espressif/tools/activate_idf_v6.0.2.sh >/dev/null 2>&1; idf.py build'
```

A compile-check needs `main/secrets.h`. Copy it from the example if it's missing. Building is the only check available without a board. Flashing and monitoring need the ESP connected, so ask the user.
