#!/bin/zsh
# STM32N6570-DK 디버그 시리얼(115200) 보기. 종료: Ctrl-A 그다음 K, y
PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1); [ -n "$PORT" ] || { echo "ST-LINK 시리얼 포트 없음"; exit 1; }
exec screen "$PORT" 115200
