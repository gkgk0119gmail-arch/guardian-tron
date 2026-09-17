#!/usr/bin/env bash
# Shared setup sourced by run_*.sh. Not meant to be executed directly.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
GATEKEEPER_DIR="$PROJECT_ROOT/gatekeeper"
GATEKEEPER_SIM_BIN="$GATEKEEPER_DIR/build/gatekeeper_sim"
SIM_LINK_PATH="/tmp/stm32_gatekeeper"
REAL_UART_PORT="/dev/ttyUSB0"  # confirmed: USB-TTL adapter <-> STM32N6570-DK D0/D1 (USART2)

export PYTHONPATH="$PROJECT_ROOT:${PYTHONPATH:-}"

build_gatekeeper_if_needed() {
    if [[ ! -x "$GATEKEEPER_SIM_BIN" ]]; then
        echo "[_common] building gatekeeper SIM binary..."
        make -C "$GATEKEEPER_DIR" sim
    fi
}

GATEKEEPER_PID=""

# Starts the local SIM gatekeeper (gatekeeper_sim, a Linux PTY standing in
# for the STM32's UART) -- use this until real firmware is flashed on the
# board (see docs/STM32_PORT.md). Same protocol bytes either way, so
# scripts don't change when you switch to --real.
start_gatekeeper_local() {
    build_gatekeeper_if_needed
    rm -f "$SIM_LINK_PATH"
    "$GATEKEEPER_SIM_BIN" "$SIM_LINK_PATH" &
    GATEKEEPER_PID=$!
    for _ in $(seq 1 50); do
        [[ -L "$SIM_LINK_PATH" ]] && break
        sleep 0.1
    done
    if [[ ! -L "$SIM_LINK_PATH" ]]; then
        echo "[_common] gatekeeper SIM did not publish $SIM_LINK_PATH in time" >&2
        exit 1
    fi
    echo "[_common] gatekeeper SIM running (pid=$GATEKEEPER_PID), link=$SIM_LINK_PATH"
}

stop_gatekeeper_local() {
    if [[ -n "$GATEKEEPER_PID" ]] && kill -0 "$GATEKEEPER_PID" 2>/dev/null; then
        kill -INT "$GATEKEEPER_PID" 2>/dev/null || true
        # Not `wait`: this script isn't always run with job control enabled
        # (e.g. under `timeout` / non-interactive invocation), where `wait`
        # on a backgrounded PID can block indefinitely instead of returning
        # once the process exits. A short poll is simpler and reliable.
        for _ in $(seq 1 20); do
            kill -0 "$GATEKEEPER_PID" 2>/dev/null || break
            sleep 0.1
        done
        kill -9 "$GATEKEEPER_PID" 2>/dev/null || true
    fi
}
