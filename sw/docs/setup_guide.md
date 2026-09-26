# Setup Guide

How to rebuild and flash HIBIKI / Guardian-TRON from scratch. If you only want to run the ready-made image, the board is shipped already flashed. Go to [operation_procedure.md](operation_procedure.md). To re-flash without building, use the files in `sw/binaries/` (step 4).

## 1. What was used

| Item | Version |
|---|---|
| Board | STMicroelectronics STM32N6570-DK (STM32N657X0H3Q, Cortex-M55 800 MHz + Neural-ART NPU) with the MB1854 camera module (IMX335 + VL53L5 ToF + LSM6DSL-class IMU) |
| RTOS | **μT-Kernel 3.0 BSP2** for STM32N657, TRON Forum, release 2026/06 (`mtkernel` 3.00). Included in `sw/guardian_vision/mtk3_bsp2/`. Original: TRON Forum μT-Kernel 3.0 BSP2 STM32N6 package (`mtk3bsp2_stm32n657.zip`), <https://github.com/tron-forum/mtk3_bsp2> |
| Development PC | macOS 26.5 on Apple Silicon (what we used). Windows and Linux work with the same tools (see notes) |
| IDE / toolchain | STM32CubeIDE 2.1.1 (only for its bundled toolchain). GNU Tools for STM32 14.3.rel1 (arm-none-eabi-gcc 14.3.1). GNU make |
| Programmer | STM32CubeProgrammer 2.22.0 (CLI, bundled with CubeIDE). **≥ 2.21 is required** for `--align` in the signing step |
| ST software | STM32N6-GettingStarted-ObjectDetection **v2.3.1** (STM32CubeN6 1.3.0 HAL/BSP, camera middleware, STEdgeAI 4.0.0 runtime, the pre-built model and the FSBL `ai_fsbl.hex` v1.4.0) |
| AI model | `st_yolo_x_nano_480_1.0_0.25_3_st_int8`: ST's pre-trained YOLOX-nano, person class, INT8, 480×480×3, compiled for the Neural-ART NPU by ST (STEdgeAI 4.0.0). We did not retrain it |
| Jetson (optional) | Jetson Orin Nano, L4T R36.4.7 (JetPack 6), ROS 2 Humble, F1TENTH stack, pyserial 3.5 |

## 2. Get the sources

```bash
git clone https://github.com/gkgk0119gmail-arch/guardian-tron.git
cd guardian-tron/sw
# ST reference (HAL, BSP, camera middleware, NPU runtime, model, FSBL): ~660 MB, not in our repo
git clone --depth 1 -b v2.3.1 https://github.com/STMicroelectronics/STM32N6-GettingStarted-ObjectDetection.git st_od_ref
```

The firmware Makefile expects the ST repository at `sw/st_od_ref` (`../st_od_ref` relative to `sw/guardian_vision/`). Override it with `make ST_ROOT=/path/to/it`. We do not modify any file in it.

## 3. Build

```bash
cd sw/guardian_vision
make -j8          # -> build/guardian.elf, build/guardian.bin   (runs from AXISRAM1 @ 0x34000400)
make sign         # -> build/guardian_sign.bin                  (for boot from flash @ 0x70100000)
```

- The Makefile finds the compiler and the signing tool inside `/Applications/STM32CubeIDE.app`. On Windows or Linux, pass them explicitly:
  `make GCC_PATH=<...>/gnu-tools-for-stm32.../tools/bin SIGNER=<...>/STM32_SigningTool_CLI`
- Expected size: `.text` ≈ 150 KB, `.rodata` ≈ 460 KB, `.data` + `.bss` ≈ 115 KB in AXISRAM1, `guardian_sign.bin` ≈ 640 KB.
- `make sign` runs `STM32_SigningTool_CLI -bin build/guardian.bin -nk -t ssbl -hv 2.3 --align`. `--align` puts the application at offset 0x400 of the image, where the FSBL expects it. Without it the board stays silent after boot from flash.

### Our changes to μT-Kernel 3.0 BSP2 (everything else is as distributed)

| File | Change | Why |
|---|---|---|
| `mtk3_bsp2/config/config.h` | `CNF_TIMER_PERIOD` 10 → **1** ms | 1 ms time resolution for the watchdog and the 100 Hz cyclic handler; `HAL_Delay()` in the camera driver no longer rounds up to 10 ms (vision 10 → 15 fps) |
| `mtk3_bsp2/config/config.h` | `CNF_SYSTEMAREA_END` = `0x340FC000` | Kernel memory must end below the 16 KB MSP stack; AXISRAM2+ belongs to the NPU |
| `mtk3_bsp2/config/config_bsp/stm32_cube/config_bsp.h` | `DEVCNF_USE_HAL_IIC`, `DEVCNF_USE_HAL_ADC` → 0 | The ST camera middleware owns I2C1 |
| `mtk3_bsp2/sysdepend/stm32_cube/cpu/core/armv8m/dispatch.S` | calls `gv_dsp_hook(TCB *next)` at every task switch (and `NULL` when no task is runnable) | T-Kernel/DS declares `td_hok_dsp()` in `tk/dbgspt.h`, but BSP2 ships no implementation of it (`TD_HDSP` appears nowhere outside that header) and `USE_DBGSPT` is 0 in our config, so the call would not link. Our hook gives per-task CPU time, a switch trace and the per-task MPU view (`os/gv_perf.c`, `os/gv_mpu.c`) |

## 4. Flash the board (boot from flash)

The external NOR flash holds three images:

| Address | Image | Source |
|---|---|---|
| `0x70000000` | FSBL (copies the application to RAM and starts it) | `st_od_ref/FSBL/ai_fsbl.hex` (also `sw/binaries/ai_fsbl.hex`) |
| `0x70100000` | HIBIKI firmware | `guardian_vision/build/guardian_sign.bin` (also `sw/binaries/guardian_sign.bin`) |
| `0x70380000` | NPU weights (1.15 MB) | `st_od_ref/Model/STM32N6570-DK/network_data.hex` |

1. Set **BOOT1 to the right** (development mode). Connect the ST-LINK USB-C port.
2. Program, with `EL = <CubeProgrammer>/bin/ExternalLoader/MX66UW1G45G_STM32N6570-DK.stldr`:
   ```bash
   STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -el $EL -hardRst -w st_od_ref/FSBL/ai_fsbl.hex
   STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -el $EL -hardRst -w st_od_ref/Model/STM32N6570-DK/network_data.hex
   STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -el $EL -hardRst -w guardian_vision/build/guardian_sign.bin 0x70100000
   ```
   On macOS, `tools/flash_boot.sh` does the FSBL and the application with retries and a read-back check. `tools/nn_weights.sh --write` does the weights.
3. Set **BOOT0 left, BOOT1 left** (boot from flash) and **power-cycle** the board: unplug every USB cable, including the Jetson's USB-TTL. The external loader leaves the NOR flash in a mode that only a power cycle resets.

### Development mode (RAM load, no flashing)

`tools/load_vision.sh` writes `build/guardian.bin` to `0x34000400` and starts it (BOOT1 right). The image is lost at power-off. The NPU weights must already be in the NOR flash.

## 5. Normal boot log (ST-LINK virtual COM port, 115200 8N1)

```
[hw] PSRAM init 0 mm 0 rw ok | NOR init 0 mm 0 | weights[0] 0xfc98fd05 ok
microT-Kernel Version 3.00
Guardian-TRON start (gatekeeper + camera/NPU person stop + IMU monitor)
[perf] context switch (tk_wup_tsk -> higher-priority task running, n=2000): min 416 ns avg 416 ns max 915 ns
[vesc] fw 6.6 ready                       <- "NO REPLY" if the VESC / car battery is not connected (not an error on a desk)
[mpu] on: gatekeeper state 3409c3e0..3409c460 writable by gate task only, 3 stack guards, null guard
[rtos] task set (fixed-priority preemptive, lower number = higher priority)
[rtos]   gate      8  Jetson CMD @100 Hz, kick  1 ms      HI-CRIT A    watchdog 200 ms -> brake
...
[gate] camera monitor LOST -> holding the car     <- normal until the camera is up (~5 s)
[vision] ready: person stop if conf>=60% h>=42% |x-0.5|<=35%, hold 1500 ms
[gate] camera monitor OK -> commands accepted
[vision] 15.0 fps | NPU 28515 us ...
[imu] ready: gravity in board axes (...) mg ...    <- keep the board still for 1 s after boot
[metric] ctx_switch: n=2000 mean 444ns p50 460ns p95 460ns p99 460ns max 872ns | target < 5700ns over 0 -> PASS   <- every 10 s
```

## 6. Jetson (only for the full car)

1. Jetson Orin Nano with JetPack 6 (L4T R36.4), ROS 2 Humble, the F1TENTH stack in `~/f1tenth_ws` (`urg_node`, `ackermann_mux`, `joy_teleop`, `race_stack`/`gap_follower`), `pip install pyserial`.
2. Copy `sw/jetson_stack/ros2/*` to `~/guardian/` on the Jetson.
3. USB-TTL (PL2303) → `/dev/ttyUSB0`. The PL2303 driver is not in the stock L4T kernel. We built `pl2303.ko` for 5.15.148-tegra and load it at boot (`/etc/modules-load.d/pl2303.conf`). A CP2102/FT232 adapter works with the stock kernel.
4. Do **not** run `vesc_driver`: the STM32 owns the VESC. `guardian_gap.launch.py` starts the LiDAR, the mux, the planner and `gt_bridge` without it.
5. From the development PC, `sw/run_demo.sh 0.5 30` does everything (set `JETSON=user@ip`).

Wiring, pins and baud rates: [wiring.md](../../hw/wiring.md).
