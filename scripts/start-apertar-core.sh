#!/bin/bash
set -euo pipefail

CORE_BIN="${APERTAR_CORE_BIN:-/home/pi/ApertarCore-build/apertar-core}"
MEDIA_ROOT="${APERTAR_MEDIA_ROOT:-/media/RAW}"
SOCKET_PATH="${APERTAR_SOCKET_PATH:-/tmp/apertar-core.sock}"

if [[ ! -x "$CORE_BIN" ]]; then
    echo "ApertarCore binary not found or not executable: $CORE_BIN" >&2
    exit 1
fi

# Saved custom selections take precedence. Otherwise Core selects the sensor's
# JSON in defaults-pisp (Pi 5) or defaults-vc4 (Pi 3/4) under
# /home/pi/Tuning-Files before libcamera starts. Core validates custom targets too.
if [[ -n "${APERTAR_TUNING_FILE:-}" ]]; then
    export LIBCAMERA_RPI_TUNING_FILE="$APERTAR_TUNING_FILE"
fi

# The pipeline config the live Core runs with (FE queue depth 4, drop-frames recipe row 8), when the rig has it; an
# env of the same name wins. libcamera opens this as a path: a bare file name is not found and it falls back to its
# defaults (depth 2) with only a warning.
PIPELINE_CONFIG=/home/pi/Tuning-Files/pipeline/pisp-depth4.yaml
if [[ -z "${LIBCAMERA_RPI_CONFIG_FILE:-}" && -f "$PIPELINE_CONFIG" ]]; then
    export LIBCAMERA_RPI_CONFIG_FILE="$PIPELINE_CONFIG"
fi

# Dual sensor: APERTAR_CAMERA picks the camera by port (cam0, cam1, or an I2C node such as i2c@88000), and
# APERTAR_WEB_PORT the web port. With neither set the command line below is just --socket and --media.
EXTRA_ARGS=()
if [[ -n "${APERTAR_CAMERA:-}" ]]; then
    EXTRA_ARGS+=(--camera "$APERTAR_CAMERA")
fi
if [[ -n "${APERTAR_WEB_PORT:-}" ]]; then
    EXTRA_ARGS+=(--web-port "$APERTAR_WEB_PORT")
fi

# APERTAR_RT=1: run Core under SCHED_FIFO (APERTAR_RT_PRIORITY, default 70) on the cores in
# APERTAR_RT_CPUS (default 1-3, empty = no pinning) with the best-effort I/O class, the way
# cinemate starts cinepi-raw (src/module/cinepi_multi.py run()). The unit needs LimitRTPRIO
# (config/apertar-core-capture-priority.conf). When chrt is refused Core starts as before.
# Off by default: the A/B in the kb (working/dng-chain) decides whether it stays.
PREFIX=()
if [[ "${APERTAR_RT:-0}" == "1" ]]; then
    RT_PRIORITY="${APERTAR_RT_PRIORITY:-70}"
    RT_CPUS="${APERTAR_RT_CPUS-1-3}"
    if command -v chrt >/dev/null 2>&1 && chrt -f "$RT_PRIORITY" /bin/true 2>/dev/null; then
        PREFIX+=(chrt -f "$RT_PRIORITY")
        if command -v ionice >/dev/null 2>&1; then
            PREFIX+=(ionice -c2 -n0)
        fi
        if [[ -n "$RT_CPUS" ]] && command -v taskset >/dev/null 2>&1; then
            PREFIX+=(taskset -c "$RT_CPUS")
        fi
        echo "ApertarCore start: real-time scheduling on: ${PREFIX[*]}" >&2
    else
        echo "ApertarCore start: APERTAR_RT=1 but chrt -f $RT_PRIORITY is refused (is LimitRTPRIO set on the unit?); starting normally" >&2
    fi
fi

mkdir -p "$(dirname "$SOCKET_PATH")"
exec ${PREFIX[@]+"${PREFIX[@]}"} "$CORE_BIN" --socket "$SOCKET_PATH" --media "$MEDIA_ROOT" ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}
