# Third-party Software, Models and Data

Contest rule 1.3: the name, rights holder, how it was obtained, what it does in HIBIKI, and its license, for everything we did not write. All items are free of charge and remain obtainable from the listed sources during judging and for at least one week after the award ceremony.

## On the STM32N6570-DK (the evaluated program)

| Name | Rights holder | Obtained from | Function in HIBIKI | License |
|---|---|---|---|---|
| **μT-Kernel 3.0 BSP2** (micro T-Kernel 3.00, STM32N657 port) | Ken Sakamura / TRON Forum | TRON Forum μT-Kernel 3.0 BSP2 package for STM32N6 (`mtk3bsp2_stm32n657.zip`), <https://github.com/tron-forum/mtk3_bsp2> | Real-time OS: tasks, cyclic handler, event flags, mutex (priority inheritance), dispatcher. Three changes, listed in [setup_guide.md](setup_guide.md) | T-License 2.2 (some files 2.1) |
| STM32N6xx HAL/LL drivers, CMSIS device files | STMicroelectronics, Arm | STM32CubeN6 1.3.0, inside STM32N6-GettingStarted-ObjectDetection v2.3.1, <https://github.com/STMicroelectronics/STM32N6-GettingStarted-ObjectDetection> | Clocks, GPIO, I2C, UART (console), DCMIPP, LTDC, DMA2D, XSPI, caches | BSD-3-Clause / Apache-2.0 |
| CMSIS Core | Arm Limited | same package | Cortex-M55 core access (DWT cycle counter, MPU, NVIC) | Apache-2.0 |
| STM32N6570-DK BSP + components (aps256xx PSRAM, mx66uw1g45g NOR, LCD, fonts) | STMicroelectronics | same package | External PSRAM/NOR, LCD | BSD-3-Clause |
| STM32 camera middleware (`stm32-mw-camera`, IMX335 driver, ISP library) | STMicroelectronics | same package | IMX335 camera capture, ISP auto-exposure/white balance | SLA0044 (sensor drivers BSD-3-Clause) |
| ST Edge AI runtime (`stedgeai-lib`, LL_ATON, NPU cache) 4.0.0 | STMicroelectronics | same package | Runs the network on the Neural-ART NPU | SLA0044 |
| Vision post-processing (`stm32-vision-models-postprocessing`, `ai-postprocessing-wrapper`) | STMicroelectronics | same package | YOLOX output decoding + NMS | SLA0044 |
| Object-detection application sources (`main.c`, `app_camerapipeline.c`, `crop_img.c`), adapted by us into `guardian_vision/Src/` | STMicroelectronics (our modifications marked "Guardian-TRON") | same package | Camera + NPU pipeline, run as a μT-Kernel task instead of the bare-metal main loop | SLA0044 |
| **Model `st_yolo_x_nano_480_1.0_0.25_3_st_int8`** (network.c, network_data.hex) | STMicroelectronics (ST Model Zoo) | same package, `Model/STM32N6570-DK` | Person detector on the NPU (INT8, 480×480). Used as delivered, not retrained | SLA0044 |
| YOLOX architecture | Megvii (Z. Ge et al., 2021) | via the ST Model Zoo | Detector architecture the ST model is based on | Apache-2.0 |
| Training data of the model: COCO 2017 (person class) | COCO Consortium | used by ST to train the model; we did not use it directly | — | annotations CC BY 4.0; images under Flickr terms |
| FSBL `ai_fsbl.hex` v1.4.0 | STMicroelectronics | same package, `FSBL/` | First-stage boot loader: copies our firmware from NOR flash to RAM at boot | SLA0044 |
| newlib-nano (C library) | Red Hat and others | bundled with GNU Tools for STM32 14.3 | `vsnprintf`, `memcpy`, math | BSD-style (newlib licenses) |

## Tools (not part of the delivered program)

| Name | Rights holder | Function | License |
|---|---|---|---|
| STM32CubeIDE 2.1.1 (GNU Tools for STM32 14.3.rel1) | STMicroelectronics / FSF | compiler, linker | SLA0048 / GPL-3.0 with runtime exception |
| STM32CubeProgrammer 2.22.0, STM32_SigningTool_CLI | STMicroelectronics | flashing, image signing | SLA0048 |
| VESC Tool | Benjamin Vedder | one-time VESC configuration (UART 38400) | GPL-3.0 |
| `stress` | Amos Waterland | CPU load for the Jetson comparison test | GPL-2.0 |

## On the Jetson (vehicle only)

| Name | Rights holder | Obtained from | Function | License |
|---|---|---|---|---|
| NVIDIA JetPack 6 / L4T R36.4.7 | NVIDIA | NVIDIA SDK Manager | Jetson OS | NVIDIA license + GPL-2.0 (kernel) |
| ROS 2 Humble | Open Robotics | apt (packages.ros.org) | middleware | Apache-2.0 |
| `urg_node` | ROS community | apt | Hokuyo LiDAR driver | BSD-3-Clause |
| F1TENTH stack (`ackermann_mux`, `joy_teleop`, `gap_follower`) | F1TENTH community / our lab | f1tenth GitHub + our lab's race stack | path planning from LiDAR | MIT / BSD / Apache-2.0 |
| pyserial 3.5 | Chris Liechti | pip | serial port access (`gt_bridge.py`, `pc_host.py`) | BSD-3-Clause |
| Linux `pl2303` USB-serial driver | Linux kernel contributors | kernel 5.15 source, built as a module | USB-TTL adapter | GPL-2.0 |
| VESC firmware 6.6 (on the motor controller, unmodified) | Benjamin Vedder | pre-installed | motor/servo control, UART protocol | GPL-3.0 |

## Our code

`guardian_vision/os/*`, `guardian_vision/gt/*`, `jetson_stack/*`, `tools/*`, `run_demo.sh` and `docs/*` are ours, released under the MIT License. The gatekeeper protocol/envelope core (`gt/gatekeeper_core.c`, `gt/protocol.c`, `gt/safety_envelope.c`) and the Jetson threat scripts come from the team's earlier SDV safety co-processor project.
