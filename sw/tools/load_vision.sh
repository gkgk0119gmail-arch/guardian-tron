#!/bin/zsh
# Guardian-TRON vision firmware (guardian_vision/build/guardian.bin) -> AXISRAM1, run from 0x34000400.
# No FSBL needed in dev mode: the app sets its own clocks. BOOT1 = dev (right, position 3).
# NN weights must already be in the external NOR at 0x70380000 (see nn_weights.sh).
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${1:-$HERE/../guardian_vision/build/guardian.bin}"
CP=$(ls /Applications/STM32CubeIDE.app/Contents/Eclipse/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.macosaarch64*/tools/bin/STM32_Programmer_CLI | head -1)
[ -f "$BIN" ] || { echo "❌ $BIN missing — run make in guardian_vision"; exit 1; }
pkill -f ST-LINK_gdbserver 2>/dev/null
for i in {1..8}; do
  "$HERE/.venv/bin/python" "$HERE/stlink_reset.py" >/dev/null || true
  OUT=$("$CP" -c port=SWD mode=UR ap=1 freq=8000 -w "$BIN" 0x34000400 -s 0x34000400 2>&1 | sed 's/\x1b\[[0-9;]*m//g')
  if echo "$OUT" | grep -q "Error"; then echo "attempt $i failed: $(echo "$OUT" | grep -m1 -E "Error" | cut -c1-90)"; sleep 1; continue; fi
  echo "✅ guardian vision loaded + running (attempt $i, $(( $(stat -f%z "$BIN") / 1024 )) KB)"
  exit 0
done
echo "❌ failed 8 times. Unplug and replug ST-LINK USB"; exit 1
