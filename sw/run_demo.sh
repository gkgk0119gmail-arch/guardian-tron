#!/bin/zsh
# Guardian-TRON person-stop demo, one command.
#
#   ./run_demo.sh [speed_mps=0.8] [seconds=15] [--no-load] [--gap] [--keep] [--stress] [--freeze]
#
#   --stress : Jetson CPU/IO/memory at 100 % (stress, 6 cores) while armed -> the STM32
#              metrics must not change (fault injection: overloaded AI computer)
#   --freeze : halfway through, freeze the Jetson bridge process for 1.5 s (SIGSTOP/SIGCONT)
#              -> STM32 watchdog must put the car in the safe state, then recover
#
#   1. loads the STM32 firmware (μT-Kernel gatekeeper + camera/NPU person detector)
#   2. starts the Jetson stack: cruise (straight, constant speed) or --gap (gap_follower)
#   3. arms for <seconds>, then disarms; the STM32 stops the car on its own when a
#      person appears in front of the camera (Jetson keeps commanding "go")
#   4. prints every STOP event with its latency breakdown
#
# Env: JETSON=orin@<ip> (default orin@172.24.121.67). Car on the floor, someone next to it.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SPEED=0.8; SECS=15; LOAD=1; CTRL=cruise; KEEP=0; STRESS=0; FREEZE=0
for a in "$@"; do
  case "$a" in
    --no-load) LOAD=0 ;; --gap) CTRL=gap ;; --keep) KEEP=1 ;; --stress) STRESS=1 ;; --freeze) FREEZE=1 ;;
    *) if [ "$SPEED" = 0.8 ] && [ "$a" != "" ] && [ -z "${SPEED_SET:-}" ]; then SPEED=$a; SPEED_SET=1; else SECS=$a; fi ;;
  esac
done
JETSON=${JETSON:-orin@172.24.121.67}
STOP_STACK="[ -f /tmp/gg.pid ] && kill -TERM -- -\$(cat /tmp/gg.pid) 2>/dev/null; sleep 2
for p in '^/usr/bin/python3 /opt/ros/humble/bin/ros2 launch /home/orin/guardian/' '^/opt/ros/humble/lib/urg_node/urg_node_driver' '^/usr/bin/python3 /home/orin/f1tenth_ws/install/race_stack/lib/race_stack/gap_follower' '^python3 /home/orin/guardian/gt_' '^/home/orin/f1tenth_ws/install/ackermann_mux/lib/ackermann_mux/ackermann_mux' '^/opt/ros/humble/lib/joy/joy_node' '^/usr/bin/python3 /opt/ros/humble/lib/joy_teleop/joy_teleop' '^/opt/ros/humble/lib/tf2_ros/static_transform_publisher 0.27'; do pkill -TERM -f \"\$p\"; done; sleep 1"
SSH=(ssh -n -o ConnectTimeout=8 -o BatchMode=yes $JETSON)
ROS="source /opt/ros/humble/setup.bash; source ~/f1tenth_ws/install/setup.bash 2>/dev/null"
LOG="$HERE/demo_logs/$(date +%Y%m%d_%H%M%S)"; mkdir -p "$LOG"
PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
[ -n "$PORT" ] || { echo "❌ STM32 ST-LINK port not found (connect USB to the Mac)"; exit 1; }

cleanup() { [ -n "${CAP:-}" ] && kill $CAP 2>/dev/null; }
trap cleanup EXIT

echo "▶ [1/4] STM32 firmware + console capture → $LOG/stm32.log"
if [ $LOAD = 1 ]; then "$HERE/tools/load_vision.sh" || exit 1; sleep 1; fi
# capture starts AFTER the load: the loader's ST-LINK USB resets re-enumerate the VCP
PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
( stty 115200 cs8 -cstopb -parenb raw -echo; exec cat ) < "$PORT" > "$LOG/stm32.log" 2>/dev/null &
CAP=$!
if [ $LOAD = 1 ]; then
  echo -n "  waiting for vision ready"
  for i in {1..40}; do grep -aq "\[vision\] ready" "$LOG/stm32.log" && break; grep -aq "init failed\|assertion" "$LOG/stm32.log" && break; echo -n "."; sleep 1; done; echo
  grep -aE "vesc\]|vision\] ready|init failed|assertion|context switch" "$LOG/stm32.log" | sed 's/^/  /'
  grep -aq "\[vision\] ready" "$LOG/stm32.log" || { echo "❌ vision did not come up. log: $LOG/stm32.log"; exit 1; }
fi

echo "▶ [2/4] Jetson stack ($CTRL, ${SPEED} m/s) @ $JETSON"
"${SSH[@]}" "$ROS
timeout 8 ros2 topic pub --once /race/enabled std_msgs/msg/Bool '{data: false}' --qos-durability transient_local >/dev/null 2>&1
$STOP_STACK
[ -e /dev/ttyUSB0 ] || { echo 'NO /dev/ttyUSB0'; exit 3; }
setsid nohup ros2 launch ~/guardian/guardian_gap.launch.py controller:=$CTRL speed:=$SPEED max_speed:=$SPEED > /tmp/gg.log 2>&1 < /dev/null & echo \$! > /tmp/gg.pid
sleep 10
# duplicates checked on real processes (the ros2 daemon caches dead node names);
# patterns are anchored so they never match this ssh shell's own command line
U='^/opt/ros/humble/lib/urg_node/urg_node_driver'; M='^/home/orin/f1tenth_ws/install/ackermann_mux/lib/ackermann_mux/ackermann_mux'
B='^python3 /home/orin/guardian/gt_bridge'; L='^/usr/bin/python3 /opt/ros/humble/bin/ros2 launch /home/orin/guardian'
C='^(python3 /home/orin/guardian/gt_cruise|/usr/bin/python3 /home/orin/f1tenth_ws/install/race_stack/lib/race_stack/gap_follower)'
dup=0; for p in \"\$U\" \"\$M\" \"\$B\" \"\$L\"; do [ \$(pgrep -fc \"\$p\") -gt 1 ] && dup=1; done
echo \"  procs: launch=\$(pgrep -fc \"\$L\") urg=\$(pgrep -fc \"\$U\") mux=\$(pgrep -fc \"\$M\") ctrl=\$(pgrep -fc \"\$C\") bridge=\$(pgrep -fc \"\$B\")\"
[ \$dup = 0 ] || { echo '  DUPLICATE PROCESSES'; exit 4; }
grep -a gt_bridge /tmp/gg.log | tail -1 | sed 's/.*\] /  /'" || { echo "❌ Jetson stack failed to start"; exit 1; }

echo "▶ [3/4] driving ${SECS} s — get ready to step in front of the car"
for c in 3 2 1; do echo "  $c..."; sleep 1; done
"${SSH[@]}" "$ROS; timeout 8 ros2 topic pub --once /race/enabled std_msgs/msg/Bool '{data: true}' --qos-durability transient_local >/dev/null 2>&1"
echo "  🚗 ARMED ($(date +%T))"
if [ $STRESS = 1 ]; then
  "${SSH[@]}" "nohup stress --cpu 6 --io 2 --vm 2 --vm-bytes 256M --timeout $SECS >/dev/null 2>&1 &"
  echo "  🔥 Jetson stress ON (cpu 6, io 2, vm 2) for ${SECS}s"
fi
if [ $FREEZE = 1 ]; then
  H=$((SECS / 2)); sleep $H
  "${SSH[@]}" "pkill -STOP -f '^python3 /home/orin/guardian/gt_bridge'; sleep 1.5; pkill -CONT -f '^python3 /home/orin/guardian/gt_bridge'"
  echo "  🧊 Jetson bridge frozen 1.5 s ($(date +%T)) -> resumed"
  sleep $((SECS - H))
else
  sleep $SECS
fi
"${SSH[@]}" "$ROS; timeout 8 ros2 topic pub --once /race/enabled std_msgs/msg/Bool '{data: false}' --qos-durability transient_local >/dev/null 2>&1"
echo "  🛑 DISARMED ($(date +%T))"
sleep 2
"${SSH[@]}" "cat /tmp/gg.log" > "$LOG/jetson.log" 2>/dev/null
if [ $KEEP = 0 ]; then "${SSH[@]}" "$STOP_STACK"; fi

echo "▶ [4/4] Results"
STOPS=$(grep -ac "STOP #" "$LOG/stm32.log")
echo "  STM32 person-detect stops: ${STOPS}"
LC_ALL=C tr -d "\r" < "$LOG/stm32.log" | grep -a -E "STOP #|hazard clear|STOP: |SLOW|camera monitor|assert|VISION STALL|FAULT INJECTION" | sed 's/^/    /'
grep "\[vision\]" "$LOG/stm32.log" | grep fps | tail -2 | sed 's/^/  /'
LC_ALL=C tr -d "\r" < "$LOG/stm32.log" | grep -a -E "^\[perf\] (cpu|gate)|^\[imu\] roll|context switch" | tail -5 | sed 's/^/  /'
grep -a gt_bridge "$LOG/jetson.log" | grep link= | tail -1 | sed 's/.*\] /  bridge: /'
LC_ALL=C tr -d "\r" < "$LOG/stm32.log" | grep -a -E "^\[fault\]" | sed 's/^/  /'
# machine-readable results: last metrics table + stack high-water marks
{
  echo "metric,n,mean,p50,p95,p99,max,target,over_target,verdict"
  LC_ALL=C tr -d "\r" < "$LOG/stm32.log" | grep -a "^\[metric\] " | tail -9 | perl -ne \
    'print "$1,$2,$3,$4,$5,$6,$7,$8,$9,$10\n" if /^\[metric\] (\w+): n=(\d+) mean (\S+) p50 (\S+) p95 (\S+) p99 (\S+) max (\S+) \| target < (\S+) over (\d+) -> (\w+)/'
} > "$LOG/metrics.csv"
LC_ALL=C tr -d "\r" < "$LOG/stm32.log" | grep -a "^\[stack\]" | tail -1 > "$LOG/stack.txt"
echo "  metrics table: $LOG/metrics.csv"
echo "  logs: $LOG"
