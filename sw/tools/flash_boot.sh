#!/bin/zsh
# Guardian-TRON standalone boot: program the external NOR so the board runs without a PC.
#   0x70000000  ST ai_fsbl (copies the app from flash to AXISRAM1 and jumps to it)
#   0x70100000  guardian_sign.bin (make sign in guardian_vision)
#   0x70380000  NN weights (already there; see nn_weights.sh)
# Board must be in DEV mode (BOOT1 = dev) while programming. Afterwards:
#   1. set BOOT1 to flash-boot,  2. power-cycle the board completely.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$HERE/../guardian_vision/build/guardian_sign.bin"
FSBL="$HERE/../st_od_ref/FSBL/ai_fsbl.hex"
CPD=$(ls -d /Applications/STM32CubeIDE.app/Contents/Eclipse/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.macosaarch64*/tools/bin | head -1)
CP="$CPD/STM32_Programmer_CLI"; EL="$CPD/ExternalLoader/MX66UW1G45G_STM32N6570-DK.stldr"
[ -f "$APP" ] || { echo "❌ $APP missing — run make sign in guardian_vision"; exit 1; }
pkill -f ST-LINK_gdbserver 2>/dev/null

run() {	# retry a CubeProgrammer call through the flaky ST-LINK USB
  for i in {1..6}; do
    "$HERE/.venv/bin/python" "$HERE/stlink_reset.py" >/dev/null || true
    OUT=$("$CP" -c port=SWD mode=HOTPLUG -el "$EL" -hardRst "$@" 2>&1 | sed 's/\x1b\[[0-9;]*m//g')
    echo "$OUT" | grep -qE "^Error:" || return 0
    echo "   attempt $i failed: $(echo "$OUT" | grep -m1 -E "^Error:" | cut -c1-90)"; sleep 1
  done
  return 1
}

echo "▶ FSBL → 0x70000000";            run -w "$FSBL" || { echo "❌ FSBL programming failed"; exit 1; }
echo "▶ app → 0x70100000 ($(( $(stat -f%z "$APP") / 1024 )) KB)"; run -w "$APP" 0x70100000 || { echo "❌ app programming failed"; exit 1; }
TMP=$(mktemp -d)
echo "▶ verify (first + last 64 KB)"
SZ=$(stat -f%z "$APP"); ok=1
for off in 0 $(( (SZ - 65536) / 4 * 4 )); do
  run -u $(printf "0x%X" $((0x70100000 + off))) 65536 "$TMP/r.bin" || ok=0
  dd if="$APP" of="$TMP/a.bin" bs=1 skip=$off count=65536 2>/dev/null
  cmp -s "$TMP/r.bin" "$TMP/a.bin" || ok=0
done
[ $ok = 1 ] || { echo "❌ readback mismatch"; exit 1; }
echo "✅ flash boot image OK → move BOOT1 to flash, then fully power-cycle (unplug and replug both ST-LINK USB + Jetson USB-TTL)"
