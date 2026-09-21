#!/bin/zsh
# View the STM32N6570-DK debug serial (115200). Quit: Ctrl-A, then K, y
PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1); [ -n "$PORT" ] || { echo "ST-LINK serial port not found"; exit 1; }
exec screen "$PORT" 115200
