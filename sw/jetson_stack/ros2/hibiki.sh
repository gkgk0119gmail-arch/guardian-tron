#!/bin/bash
# hibiki.sh - one-word commands on the Jetson for evaluating HIBIKI / Guardian-TRON.
#
#   ~/guardian/hibiki.sh start [speed_mps=0.3] [cruise|gap]   start the ROS 2 stack (LiDAR, planner, gt_bridge)
#   ~/guardian/hibiki.sh arm                                  the car starts driving (commands go to the STM32)
#   ~/guardian/hibiki.sh disarm                               the planner sends speed 0
#   ~/guardian/hibiki.sh stress [secs=30]                     all 6 CPU cores at 100 % (fault injection: overloaded AI)
#   ~/guardian/hibiki.sh freeze [secs=1.5]                    freeze the command bridge (fault injection: frozen AI)
#   ~/guardian/hibiki.sh status                               what the bridge sees (verdict counts, latency)
#   ~/guardian/hibiki.sh stop                                 stop everything
#
# The STM32 owns the motor: never start vesc_driver next to this stack.
source /opt/ros/humble/setup.bash
source ~/f1tenth_ws/install/setup.bash 2>/dev/null

BRIDGE='^python3 /home/orin/guardian/gt_bridge'
enabled() {
  timeout 8 ros2 topic pub --once /race/enabled std_msgs/msg/Bool "{data: $1}" \
    --qos-durability transient_local >/dev/null 2>&1
}
stop_stack() {
  enabled false
  [ -f /tmp/gg.pid ] && kill -TERM -- -"$(cat /tmp/gg.pid)" 2>/dev/null
  sleep 2
  for p in '^/usr/bin/python3 /opt/ros/humble/bin/ros2 launch /home/orin/guardian/' \
           '^/opt/ros/humble/lib/urg_node/urg_node_driver' \
           '^/usr/bin/python3 /home/orin/f1tenth_ws/install/race_stack/lib/race_stack/gap_follower' \
           '^python3 /home/orin/guardian/gt_' \
           '^/home/orin/f1tenth_ws/install/ackermann_mux/lib/ackermann_mux/ackermann_mux' \
           '^/opt/ros/humble/lib/joy/joy_node' \
           '^/usr/bin/python3 /opt/ros/humble/lib/joy_teleop/joy_teleop' \
           '^/opt/ros/humble/lib/tf2_ros/static_transform_publisher 0.27'; do
    pkill -TERM -f "$p"
  done
  rm -f /tmp/gg.pid
}
status() {
  if [ ! -e /dev/ttyUSB0 ]; then echo "  !! /dev/ttyUSB0 missing: unplug and replug the USB-TTL cable"; fi
  line=$(grep -a gt_bridge /tmp/gg.log 2>/dev/null | grep link= | tail -1 | sed 's/.*\] /  /')
  echo "${line:-  bridge not running (hibiki.sh start)}"
}

case "${1:-}" in
  start)
    speed=${2:-0.3}; ctrl=${3:-cruise}
    stop_stack
    [ -e /dev/ttyUSB0 ] || { echo "  !! /dev/ttyUSB0 missing: unplug and replug the USB-TTL cable"; exit 1; }
    setsid nohup ros2 launch ~/guardian/guardian_gap.launch.py controller:="$ctrl" speed:="$speed" max_speed:="$speed" \
      > /tmp/gg.log 2>&1 < /dev/null & echo $! > /tmp/gg.pid
    echo "  starting ($ctrl, $speed m/s) ..."; sleep 10; status
    echo "  next: hibiki.sh arm" ;;
  arm)    enabled true;  echo "  ARMED: the car drives. Stop: hibiki.sh disarm" ;;
  disarm) enabled false; echo "  DISARMED" ;;
  stress)
    secs=${2:-30}; echo "  Jetson CPU at 100 % for $secs s (stress --cpu 6 --io 2 --vm 2)"
    stress --cpu 6 --io 2 --vm 2 --vm-bytes 256M --timeout "$secs" >/dev/null 2>&1; echo "  stress done" ;;
  freeze)
    secs=${2:-1.5}; echo "  freezing the command bridge for $secs s: the STM32 must brake after 200 ms"
    pkill -STOP -f "$BRIDGE"; sleep "$secs"; pkill -CONT -f "$BRIDGE"; echo "  bridge resumed" ;;
  status) status ;;
  stop)   stop_stack; echo "  stopped" ;;
  *) sed -n '2,12p' "$0"; exit 1 ;;
esac
