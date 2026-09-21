#!/bin/zsh
# μT-Kernel 3.0 BSP2 / STM32N6570-DK : load FSBL + Appli (Debug build) into RAM and run. No IDE needed.
# Prereqs: BOOT1=1 (dev boot), ST-LINK USB connected, FSBL/Appli built in CubeIDE
# ST-LINK V3 USB intermittently hangs on the Mac (device not responding): auto reset + up to 8 retries
set -u
# ⚠️ NEVER connect with mode=HOTPLUG: a HOTPLUG attach while the app runs makes the STM32N6 lock debug (TrustZone DA) and every connect fails until a power cycle.
#    Always use mode=UR (connect under reset) only. After a failure, unplug and replug both ST-LINK USB and Jetson USB-TTL.
HERE="$(cd "$(dirname "$0")" && pwd)"; PRJ="$HERE/../mtk3bsp2_stm32n657"
CP=$(ls /Applications/STM32CubeIDE.app/Contents/Eclipse/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.macosaarch64*/tools/bin/STM32_Programmer_CLI | head -1)
APPLI="${1:-$PRJ/Appli/Debug/mtk3bsp2_stm32n657_Appli.bin}"; FSBL="$PRJ/FSBL/Debug/mtk3bsp2_stm32n657_FSBL.bin"   # another Appli .bin can be passed as an argument
[ -f "$APPLI" ] && [ -f "$FSBL" ] || { echo "❌ .bin missing — Build Project for FSBL and Appli in CubeIDE first"; exit 1; }
pkill -f ST-LINK_gdbserver 2>/dev/null
for i in {1..8}; do
  "$HERE/.venv/bin/python" "$HERE/stlink_reset.py" >/dev/null || true
  OUT=$("$CP" -c port=SWD mode=UR ap=1 freq=8000 \
        -w "$APPLI" 0x34000400 -w "$FSBL" 0x34180400 -s 0x34180400 2>&1 | sed 's/\x1b\[[0-9;]*m//g')
  if echo "$OUT" | grep -q "Error"; then echo "attempt $i failed: $(echo "$OUT" | grep -m1 -E "Error" | cut -c1-90)"; sleep 1; continue; fi
  echo "✅ loaded + running (attempt $i)"; echo "$OUT" | grep -E "Device name|elapsed" | sed 's/^/   /'
  echo "serial console: $HERE/serial.sh"; exit 0
done
echo "❌ failed 8 times. Replug the cable or use another port/cable"; exit 1
