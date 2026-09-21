#!/usr/bin/env python3
"""gt_cruise - constant-speed straight driving for the Guardian-TRON person-stop demo.

Publishes /drive (speed, steering 0) at 50 Hz while /race/enabled is true. The Jetson
deliberately knows nothing about pedestrians here: stopping for the person that jumps
out is entirely the STM32's job (camera + NPU + μT-Kernel gatekeeper).
"""
import rclpy
from ackermann_msgs.msg import AckermannDriveStamped
from rclpy.node import Node
from rclpy.qos import DurabilityPolicy, QoSProfile
from std_msgs.msg import Bool

LATCHED = QoSProfile(depth=1, durability=DurabilityPolicy.TRANSIENT_LOCAL)


class Cruise(Node):
    def __init__(self):
        super().__init__('gt_cruise')
        self.speed = float(self.declare_parameter('speed', 0.8).value)
        self.steer = float(self.declare_parameter('steering', 0.0).value)
        self.enabled = False
        self.pub = self.create_publisher(AckermannDriveStamped, '/drive', 10)
        self.create_subscription(Bool, '/race/enabled', self.on_enable, LATCHED)
        self.create_timer(0.02, self.tick)
        self.get_logger().info(f'cruise {self.speed:.2f} m/s, steering {self.steer:+.3f} rad (waiting for /race/enabled)')

    def on_enable(self, msg):
        if msg.data != self.enabled:
            self.get_logger().info('ARMED' if msg.data else 'DISARMED')
        self.enabled = msg.data
        if not self.enabled:
            self.publish(0.0)

    def publish(self, v):
        m = AckermannDriveStamped()
        m.header.stamp = self.get_clock().now().to_msg()
        m.drive.speed = v
        m.drive.steering_angle = self.steer
        self.pub.publish(m)

    def tick(self):
        if self.enabled:
            self.publish(self.speed)


def main():
    rclpy.init()
    n = Cruise()
    try:
        rclpy.spin(n)
    except KeyboardInterrupt:
        pass
    finally:
        n.destroy_node()
        rclpy.try_shutdown()


if __name__ == '__main__':
    main()
