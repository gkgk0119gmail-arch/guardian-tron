#!/bin/zsh
# μT-Kernel 3.0 BSP2 / STM32N6570-DK : FSBL + Appli(Debug 빌드)를 RAM에 올리고 실행. IDE 불필요.
# 전제: BOOT1=1(개발 부팅), ST-LINK USB 연결, CubeIDE에서 FSBL/Appli Build 완료
# 맥에서 ST-LINK V3 USB가 간헐적으로 멈추므로(device not responding) 자동 리셋 + 최대 8회 재시도
set -u
# ⚠️ 절대 mode=HOTPLUG 로 붙지 말 것: 앱 실행 중 HOTPLUG 접속 시 STM32N6가 디버그를 잠그고(TrustZone DA) 전원 재투입 전까지 모든 접속 실패.
#    항상 mode=UR (connect under reset) 만 사용. 실패 후엔 ST-LINK USB + 젯슨 USB-TTL 둘 다 뽑았다 꽂기.
HERE="$(cd "$(dirname "$0")" && pwd)"; PRJ="$HERE/../mtk3bsp2_stm32n657"
CP=$(ls /Applications/STM32CubeIDE.app/Contents/Eclipse/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.macosaarch64*/tools/bin/STM32_Programmer_CLI | head -1)
APPLI="${1:-$PRJ/Appli/Debug/mtk3bsp2_stm32n657_Appli.bin}"; FSBL="$PRJ/FSBL/Debug/mtk3bsp2_stm32n657_FSBL.bin"   # 인자로 다른 Appli .bin 지정 가능
[ -f "$APPLI" ] && [ -f "$FSBL" ] || { echo "❌ .bin 없음 — CubeIDE에서 FSBL, Appli 먼저 Build Project"; exit 1; }
pkill -f ST-LINK_gdbserver 2>/dev/null
for i in {1..8}; do
  "$HERE/.venv/bin/python" "$HERE/stlink_reset.py" >/dev/null || true
  OUT=$("$CP" -c port=SWD mode=UR ap=1 freq=8000 \
        -w "$APPLI" 0x34000400 -w "$FSBL" 0x34180400 -s 0x34180400 2>&1 | sed 's/\x1b\[[0-9;]*m//g')
  if echo "$OUT" | grep -q "Error"; then echo "시도 $i 실패: $(echo "$OUT" | grep -m1 -E "Error" | cut -c1-90)"; sleep 1; continue; fi
  echo "✅ 로드 + 실행 완료 (시도 $i)"; echo "$OUT" | grep -E "Device name|elapsed" | sed 's/^/   /'
  echo "시리얼 보기: $HERE/serial.sh"; exit 0
done
echo "❌ 8회 실패. 케이블을 뽑았다 꽂거나 다른 포트/케이블 사용"; exit 1
