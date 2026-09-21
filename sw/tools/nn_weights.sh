#!/bin/zsh
# NN weights (st_yolo_x_nano_480 person) in the external NOR flash at 0x70380000.
#   ./nn_weights.sh          verify only (read back and compare)
#   ./nn_weights.sh --write  program network_data.hex if different
#
# ⚠️  The external loader leaves the NOR in OPI-DTR mode and an MCU reset does not reset the
#    chip: afterwards the firmware's NOR/PSRAM init hangs or the NPU raises a BUSIF error.
#    ALWAYS power-cycle the board (ST-LINK USB + Jetson USB-TTL out) after running this.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
MODEL="$HERE/../st_od_ref/Model/STM32N6570-DK"
BIN_REF="$MODEL/network_data.xSPI2.bin"; HEX="$MODEL/network_data.hex"
CPD=$(ls -d /Applications/STM32CubeIDE.app/Contents/Eclipse/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.macosaarch64*/tools/bin | head -1)
CP="$CPD/STM32_Programmer_CLI"; EL="$CPD/ExternalLoader/MX66UW1G45G_STM32N6570-DK.stldr"
SIZE=$(stat -f%z "$BIN_REF"); TMP=$(mktemp -d)
ok=1
for off in 0 262144 524288 786432 $((SIZE-65536)); do
  "$HERE/.venv/bin/python" "$HERE/stlink_reset.py" >/dev/null || true
  "$CP" -c port=SWD mode=UR ap=1 -el "$EL" -u $(printf "0x%X" $((0x70380000+off))) 65536 "$TMP/c.bin" >/dev/null 2>&1
  dd if="$BIN_REF" of="$TMP/r.bin" bs=1 skip=$off count=65536 2>/dev/null
  cmp -s "$TMP/c.bin" "$TMP/r.bin" || ok=0
done
if [ $ok = 1 ]; then echo "✅ 가중치 일치 (5 x 64 KB spot check @0x70380000) — 이제 보드 전원 완전히 껐다 켜기"; exit 0; fi
echo "⚠️  플래시 가중치가 다름/없음"
[ "${1:-}" = "--write" ] || { echo "   굽기: $0 --write"; exit 2; }
"$HERE/.venv/bin/python" "$HERE/stlink_reset.py" >/dev/null || true
"$CP" -c port=SWD mode=UR ap=1 -el "$EL" -w "$HEX" -v 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | grep -E "Error|Download|verified|elapsed" | head -5
