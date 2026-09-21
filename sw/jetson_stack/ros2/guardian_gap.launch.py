"""guardian_gap.launch.py - F1TENTH gap_follower driving through the Guardian-TRON STM32.

Same as f1tenth_stack bringup, except the three VESC nodes (vesc_driver,
ackermann_to_vesc, vesc_to_odom) are replaced by gt_bridge: the STM32 owns the
VESC, so every command crosses the μT-Kernel gatekeeper.

  urg_node --/scan--> gap_follower --/drive--> ackermann_mux --/ackermann_cmd--> gt_bridge --UART--> STM32 --> VESC
                                     joystick --/teleop--^ (higher priority)

gap_follower stays idle until armed:
  ros2 topic pub --once /race/enabled std_msgs/msg/Bool '{data: true}' --qos-durability transient_local
Disarm with data: false.

Args:
  controller:=gap     gap_follower (lidar obstacle avoidance), max_speed/min_speed apply
  controller:=cruise  gt_cruise: constant speed straight (person-stop demo), speed applies
  max_speed (0.6), min_speed (0.3), speed (0.8)
"""
import os

from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, ExecuteProcess
from launch.conditions import IfCondition
from launch.substitutions import PythonExpression
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description():
    cfg = os.path.join(get_package_share_directory('f1tenth_stack'), 'config')
    max_speed = LaunchConfiguration('max_speed')
    min_speed = LaunchConfiguration('min_speed')
    speed = LaunchConfiguration('speed')
    controller = LaunchConfiguration('controller')
    is_gap = IfCondition(PythonExpression(["'", controller, "' == 'gap'"]))
    is_cruise = IfCondition(PythonExpression(["'", controller, "' == 'cruise'"]))

    return LaunchDescription([
        DeclareLaunchArgument('max_speed', default_value='0.6'),
        DeclareLaunchArgument('min_speed', default_value='0.3'),
        DeclareLaunchArgument('speed', default_value='0.8'),
        DeclareLaunchArgument('controller', default_value='gap'),

        Node(package='urg_node', executable='urg_node_driver', name='urg_node',
             parameters=[os.path.join(cfg, 'sensors.yaml')]),
        Node(package='tf2_ros', executable='static_transform_publisher',
             name='static_baselink_to_laser',
             arguments=['0.27', '0.0', '0.11', '0.0', '0.0', '0.0', 'base_link', 'laser']),
        Node(package='joy', executable='joy_node', name='joy',
             parameters=[os.path.join(cfg, 'joy_teleop.yaml')]),
        Node(package='joy_teleop', executable='joy_teleop', name='joy_teleop',
             parameters=[os.path.join(cfg, 'joy_teleop.yaml')]),
        Node(package='ackermann_mux', executable='ackermann_mux', name='ackermann_mux',
             parameters=[os.path.join(cfg, 'mux.yaml')]),   # publishes /ackermann_cmd

        Node(package='race_stack', executable='gap_follower', name='gap_follower',
             output='screen', condition=is_gap,
             # max_steer 0.30 rad = 17.2 deg, just inside the gatekeeper's 17.5 deg
             # envelope, so normal commands pass and a VETO means a real violation.
             parameters=[{'max_speed': max_speed, 'min_speed': min_speed,
                          'max_steer': 0.30, 'bench': False}]),

        ExecuteProcess(cmd=['python3', os.path.expanduser('~/guardian/gt_cruise.py'),
                            '--ros-args', '-p', ['speed:=', speed]],
                       output='screen', condition=is_cruise),

        ExecuteProcess(cmd=['python3', os.path.expanduser('~/guardian/gt_bridge.py')],
                       output='screen'),
    ])
