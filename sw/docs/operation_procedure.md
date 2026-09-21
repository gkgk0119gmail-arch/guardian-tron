# Operation Procedure

Step by step, from power-off to a running system and back. What the outputs mean is in [operation_manual.md](operation_manual.md).

## A. Desk kit: STM32N6570-DK only (no car, no Jetson)

Needed: the DK board with the camera module attached (as shipped), a USB-C cable, a PC with a serial terminal (TeraTerm, PuTTY, `screen`, `minicom`) or the Python script below.

1. Check the switches: **BOOT0 left, BOOT1 left**.
2. Connect the ST-LINK USB-C port to the PC. The board powers up and boots from flash (≈ 5 s).
3. Open the ST-LINK virtual COM port at **115200 8N1**. On Windows it is a `COMx` "STMicroelectronics STLink Virtual COM Port". If the terminal stays empty, see "No log" in the operation manual.
4. **Keep the board still for 2 s** after power-on. The IMU calibrates its zero. The log prints `[imu] ready`.
5. The LCD shows the camera image. The log prints the task table (`[rtos] …`), `[vision] 15.0 fps …` every second and the `[metric]` table every 10 s.
6. Walk into the camera's view: a box is drawn around the person. Within about 2 m, the log prints `[gate] STOP #n: PERSON AHEAD …`. Further away: `[gate] SLOW …` (only while a host sends drive commands).
7. Optional, a host instead of the Jetson: USB-TTL (3.3 V) TXD → D0, RXD → D1, GND → GND, then on the PC
   `python3 sw/tools/pc_host.py --port <COMx|/dev/ttyUSB0> --speed 0 --secs 30`
   Use `--speed 0` on a desk. The VESC is not connected, so nothing moves anyway.
8. To stop: unplug the USB-C cable. Nothing needs to be saved.

## B. Full car (Jetson + VESC)

Needed: the car with the 4S LiPo, the Jetson with the ROS 2 stack (see the setup guide), the STM32 powered from its own 5 V source, a PC on the same network as the Jetson.

1. **Car on a stand** (wheels off the ground) for the first run.
2. Power order: (1) STM32 5 V (USB-C). (2) Battery → VESC, Jetson, LiDAR. (3) Wait for the Jetson to boot (≈ 60 s).
3. Check the Jetson sees the USB-TTL: `ls /dev/ttyUSB0`.
4. On the PC: `JETSON=orin@<jetson-ip> sw/run_demo.sh 0.5 30 --no-load`
   - starts the ROS 2 stack on the Jetson without `vesc_driver`,
   - arms for 30 s at 0.5 m/s (constant speed, straight),
   - prints every STOP / SLOW / fault event and the metrics, and saves everything in `demo_logs/<time>/`.
   Options: `--gap` (LiDAR gap follower instead of straight), `--stress` (Jetson CPU at 100 %), `--freeze` (Jetson frozen for 1.5 s halfway).
5. Walk toward the car: it slows from about 3.3 m and stops within about 2 m. Step aside: it continues after 1.5 s.
6. Stop: the script disarms at the end. Emergency: lift the car (tilt > 30° stops it), press USER1, or unplug the STM32. With no valid Jetson command for 200 ms, the STM32 brakes by itself.
7. Power-off order: disarm → battery off → STM32 USB off.

## C. Switching back to development mode

BOOT1 right, power-cycle, then `sw/tools/load_vision.sh` (RAM) or `sw/tools/flash_boot.sh` (flash). See the setup guide.
