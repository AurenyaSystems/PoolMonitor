#!/usr/bin/env bash
# Host tests: compile the real driver and app sources against stub ESP-IDF
# headers (stubs/) and drive them with simulated hardware. No board needed;
# any g++ works. Run before opening a PR.
#
#   test/host/run.sh            run everything
#   test/host/run.sh cli        run one (onewire_ds18b20 | ezo_sensor | cli)
set -euo pipefail
cd "$(dirname "$0")"

# An ESP-IDF-activated shell (idf.py export, the VS Code ESP-IDF terminal) puts
# the Xtensa binutils first in PATH, and the host g++ then picks up the wrong
# `as`. Put the system toolchain back in front.
export PATH="/usr/bin:/usr/local/bin:$PATH"
R=../..
OUT=build
mkdir -p "$OUT"

CXX="${CXX:-g++} -std=gnu++20 -Wall -Wno-format -Wno-unused-variable -I stubs -I $R/app/inc -I $R/core/inc -I $R/drivers/inc"

build_and_run() {
    local name=$1; shift
    echo "== $name"
    $CXX "$@" -o "$OUT/$name"
    # driver log lines are noise here; failures and the verdict are what matter
    "./$OUT/$name" | grep -v '^[IEWD] '
}

want="${1:-all}"

[[ $want == all || $want == onewire_ds18b20 ]] && build_and_run onewire_ds18b20 \
    onewire_ds18b20_test.cpp $R/core/src/onewire.cpp $R/drivers/src/ds18b20.cpp

[[ $want == all || $want == ezo_sensor ]] && build_and_run ezo_sensor \
    ezo_sensor_test.cpp $R/drivers/src/ezo_sensor.cpp

[[ $want == all || $want == cli ]] && build_and_run cli \
    cli_test.cpp $R/app/src/cli_class.cpp $R/app/src/telemetry.cpp $R/core/src/onewire.cpp \
    $R/drivers/src/ds18b20.cpp $R/drivers/src/ezo_sensor.cpp $R/drivers/src/dht_sensor.cpp \
    $R/drivers/src/seq_counter.cpp

echo "all host tests passed"
