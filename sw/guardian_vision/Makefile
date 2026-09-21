##############################################################################
# Guardian-TRON firmware: μT-Kernel 3.0 gatekeeper + camera/NPU person stop
#   base: STM32N6-GettingStarted-ObjectDetection v2.3.1 (firmware/st_od_ref)
#   make            -> build/guardian.elf / .bin   (dev mode: load to 0x34000400)
#   make sign       -> build/guardian_sign.bin     (boot from flash @0x70100000)
##############################################################################
ST_ROOT   ?= ../st_od_ref
MODEL_DIR  = $(ST_ROOT)/Model/STM32N6570-DK
TARGET     = guardian
BUILD_DIR  = build
OPT        = -Os -g3

GCC_PATH  ?= $(firstword $(wildcard /Applications/STM32CubeIDE.app/Contents/Eclipse/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32*/tools/bin))
PREFIX     = arm-none-eabi-
CC         = $(GCC_PATH)/$(PREFIX)gcc
AS         = $(GCC_PATH)/$(PREFIX)gcc -x assembler-with-cpp
CP         = $(GCC_PATH)/$(PREFIX)objcopy
SZ         = $(GCC_PATH)/$(PREFIX)size
SIGNER    ?= $(firstword $(wildcard /Applications/STM32CubeIDE.app/Contents/Eclipse/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer*/tools/bin/STM32_SigningTool_CLI))

CPU = -mcpu=cortex-m55 -mcmse -mthumb
FPU = -mfpu=fpv5-d16 -mfloat-abi=hard
MCU = $(CPU) $(FPU)

# ---------------- ST vision application (camera, NPU runtime, post-processing, BSP) ----------------
C_SOURCES += Src/st_vision.c
C_SOURCES += Src/syscalls.c
C_SOURCES += Src/app_fuseprogramming.c
C_SOURCES += $(wildcard $(ST_ROOT)/Middlewares/ai-postprocessing-wrapper/*.c)
C_SOURCES += $(wildcard $(ST_ROOT)/Middlewares/stm32-vision-models-postprocessing/lib_vision_models_pp/Src/*.c)
C_SOURCES += Src/stm32_lcd_ex.c
C_SOURCES += Src/stm32n6xx_it.c
C_SOURCES += $(ST_ROOT)/Middlewares/stedgeai-lib/Npu/Devices/STM32N6xx/mcu_cache.c
C_SOURCES += $(ST_ROOT)/Middlewares/stedgeai-lib/Npu/Devices/STM32N6xx/npu_cache.c
C_SOURCES += $(MODEL_DIR)/network.c
C_SOURCES += $(MODEL_DIR)/stai_network.c
C_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Drivers/CMSIS/Device/ST/STM32N6xx/Source/Templates/system_stm32n6xx_fsbl.c
C_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Drivers/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal.c
C_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Drivers/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_cortex.c
C_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Drivers/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_dcmipp.c
C_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Drivers/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_dma2d.c
C_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Drivers/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_gpio.c
C_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Drivers/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_uart.c
C_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Drivers/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_i2c.c
C_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Drivers/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_i2c_ex.c
C_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Drivers/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_ltdc.c
C_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Drivers/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_ltdc_ex.c
C_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Drivers/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_rif.c
C_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Drivers/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_ramcfg.c
C_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Drivers/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_cacheaxi.c
C_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Drivers/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_pwr.c
C_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Drivers/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_pwr_ex.c
C_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Drivers/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_rcc.c
C_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Drivers/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_rcc_ex.c
C_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Drivers/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_xspi.c
C_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Drivers/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_bsec.c
C_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Drivers/BSP/STM32N6570-DK/stm32n6570_discovery.c
C_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Drivers/BSP/STM32N6570-DK/stm32n6570_discovery_bus.c
C_SOURCES += $(ST_ROOT)/Middlewares/stm32-mw-camera/cmw_camera.c
C_SOURCES += $(ST_ROOT)/Middlewares/stm32-mw-camera/cmw_utils.c
C_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Drivers/BSP/STM32N6570-DK/stm32n6570_discovery_lcd.c
C_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Drivers/BSP/STM32N6570-DK/stm32n6570_discovery_xspi.c
C_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Drivers/BSP/Components/aps256xx/aps256xx.c
C_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Drivers/BSP/Components/mx66uw1g45g/mx66uw1g45g.c
C_SOURCES += $(ST_ROOT)/Middlewares/stm32-mw-camera/sensors/imx335/imx335.c
C_SOURCES += $(ST_ROOT)/Middlewares/stm32-mw-camera/sensors/imx335/imx335_reg.c
C_SOURCES += $(ST_ROOT)/Middlewares/stm32-mw-camera/sensors/vd6g/vd6g.c
C_SOURCES += $(ST_ROOT)/Middlewares/stm32-mw-camera/sensors/vd55g1/vd55g1.c
C_SOURCES += $(ST_ROOT)/Middlewares/stm32-mw-camera/sensors/vd1943/vd1943.c
C_SOURCES += $(ST_ROOT)/Middlewares/stm32-mw-camera/sensors/ov5640/ov5640.c
C_SOURCES += $(ST_ROOT)/Middlewares/stm32-mw-camera/sensors/ov5640/ov5640_reg.c
C_SOURCES += $(ST_ROOT)/Middlewares/stm32-mw-camera/sensors/cmw_vd55g1.c
C_SOURCES += $(ST_ROOT)/Middlewares/stm32-mw-camera/sensors/cmw_vd66gy.c
C_SOURCES += $(ST_ROOT)/Middlewares/stm32-mw-camera/sensors/cmw_imx335.c
C_SOURCES += $(ST_ROOT)/Middlewares/stm32-mw-camera/sensors/cmw_vd1943.c
C_SOURCES += $(ST_ROOT)/Middlewares/stm32-mw-camera/sensors/cmw_ov5640.c
C_SOURCES += Src/crop_img.c
C_SOURCES += Src/app_camerapipeline.c
C_SOURCES += $(ST_ROOT)/Middlewares/stm32-mw-camera/ISP_Library/isp/Src/isp_algo.c
C_SOURCES += $(ST_ROOT)/Middlewares/stm32-mw-camera/ISP_Library/isp/Src/isp_awb_algo.c
C_SOURCES += $(ST_ROOT)/Middlewares/stm32-mw-camera/ISP_Library/isp/Src/isp_ae_algo.c
C_SOURCES += $(ST_ROOT)/Middlewares/stm32-mw-camera/ISP_Library/isp/Src/isp_cmd_parser.c
C_SOURCES += $(ST_ROOT)/Middlewares/stm32-mw-camera/ISP_Library/isp/Src/isp_core.c
C_SOURCES += $(ST_ROOT)/Middlewares/stm32-mw-camera/ISP_Library/isp/Src/isp_services.c
C_SOURCES += $(ST_ROOT)/Middlewares/stm32-mw-camera/ISP_Library/isp/Src/isp_tool_com.c
C_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Utilities/lcd/stm32_lcd.c
C_SOURCES += $(ST_ROOT)/Middlewares/stedgeai-lib/Npu/ll_aton/ll_aton.c
C_SOURCES += $(ST_ROOT)/Middlewares/stedgeai-lib/Npu/ll_aton/ll_aton_rt_main.c
C_SOURCES += $(ST_ROOT)/Middlewares/stedgeai-lib/Npu/ll_aton/ll_aton_runtime.c
C_SOURCES += $(ST_ROOT)/Middlewares/stedgeai-lib/Npu/ll_aton/ll_aton_cipher.c
C_SOURCES += $(ST_ROOT)/Middlewares/stedgeai-lib/Npu/ll_aton/ll_aton_stai_internal.c
C_SOURCES += $(ST_ROOT)/Middlewares/stedgeai-lib/Npu/ll_aton/ll_aton_util.c
C_SOURCES += $(ST_ROOT)/Middlewares/stedgeai-lib/Npu/ll_aton/ll_sw_float.c
C_SOURCES += $(ST_ROOT)/Middlewares/stedgeai-lib/Npu/ll_aton/ll_sw_integer.c
C_SOURCES += $(ST_ROOT)/Middlewares/stedgeai-lib/Npu/ll_aton/ll_aton_lib.c
C_SOURCES += $(ST_ROOT)/Middlewares/stedgeai-lib/Npu/ll_aton/ll_aton_lib_sw_operators.c
ASM_SOURCES += $(ST_ROOT)/STM32Cube_FW_N6/Drivers/CMSIS/Device/ST/STM32N6xx/Source/Templates/gcc/startup_stm32n657xx_fsbl.s
C_DEFS += -DSTM32N657xx
C_DEFS += -DBUS_I2C1_FREQUENCY=400000U
C_DEFS += -DUSE_FULL_ASSERT
C_DEFS += -DUSE_FULL_LL_DRIVER
C_DEFS += -DVECT_TAB_SRAM
C_DEFS += -DLL_ATON_DUMP_DEBUG_API
C_DEFS += -DLL_ATON_PLATFORM=LL_ATON_PLAT_STM32N6
C_DEFS += -DLL_ATON_OSAL=LL_ATON_OSAL_BARE_METAL
C_DEFS += -DLL_ATON_RT_MODE=LL_ATON_RT_ASYNC
C_DEFS += -DLL_ATON_SW_FALLBACK
C_DEFS += -DLL_ATON_DBG_BUFFER_INFO_EXCLUDED=1
C_INCLUDES += -I$(ST_ROOT)/Middlewares/stm32-mw-camera
C_INCLUDES += -I$(ST_ROOT)/Middlewares/stm32-mw-camera/sensors
C_INCLUDES += -I$(ST_ROOT)/Middlewares/stm32-mw-camera/sensors/imx335
C_INCLUDES += -I$(ST_ROOT)/Middlewares/stm32-mw-camera/sensors/vd55g1
C_INCLUDES += -I$(ST_ROOT)/Middlewares/stm32-mw-camera/sensors/vd6g
C_INCLUDES += -I$(ST_ROOT)/Middlewares/stm32-mw-camera/sensors/vd1943
C_INCLUDES += -I$(ST_ROOT)/Middlewares/stm32-mw-camera/sensors/ov5640
C_INCLUDES += -IInc
C_INCLUDES += -I$(MODEL_DIR)
C_INCLUDES += -I$(ST_ROOT)/Middlewares/ai-postprocessing-wrapper
C_INCLUDES += -I$(ST_ROOT)/Middlewares/stm32-vision-models-postprocessing/lib_vision_models_pp/Inc
C_INCLUDES += -I$(ST_ROOT)/Middlewares/stedgeai-lib/Npu/ll_aton
C_INCLUDES += -I$(ST_ROOT)/STM32Cube_FW_N6/Drivers/STM32N6xx_HAL_Driver/Inc
C_INCLUDES += -I$(ST_ROOT)/STM32Cube_FW_N6/Drivers/STM32N6xx_HAL_Driver/Inc/Legacy
C_INCLUDES += -I$(ST_ROOT)/STM32Cube_FW_N6/Drivers/CMSIS/Device/ST/STM32N6xx/Include
C_INCLUDES += -I$(ST_ROOT)/STM32Cube_FW_N6/Drivers/CMSIS/Include
C_INCLUDES += -I$(ST_ROOT)/STM32Cube_FW_N6/Drivers/CMSIS/DSP/Include
C_INCLUDES += -I$(ST_ROOT)/STM32Cube_FW_N6/Drivers/BSP/Components/Common
C_INCLUDES += -I$(ST_ROOT)/STM32Cube_FW_N6/Drivers/BSP/STM32N6570-DK
C_INCLUDES += -I$(ST_ROOT)/Middlewares/stm32-mw-camera/ISP_Library/isp/Inc
C_INCLUDES += -I$(ST_ROOT)/Middlewares/stedgeai-lib/Inc
C_INCLUDES += -I$(ST_ROOT)/Middlewares/stedgeai-lib/Npu/Devices/STM32N6xx
C_INCLUDES += -I$(ST_ROOT)/STM32Cube_FW_N6/Utilities/lcd
C_INCLUDES += -I$(ST_ROOT)/STM32Cube_FW_N6/Drivers/BSP/Components/aps256xx

C_SOURCES += Src/app_camerapipeline.c Src/crop_img.c
C_SOURCES := $(sort $(C_SOURCES))

# ---------------- Guardian-TRON: μT-Kernel side ----------------
C_SOURCES += $(wildcard os/*.c) $(wildcard gt/*.c)
MTK_C  := $(shell find mtk3_bsp2 -name '*.c')
MTK_S  := $(shell find mtk3_bsp2 -name '*.S')
C_SOURCES += $(MTK_C)
ASM_SOURCES_S += $(MTK_S)
C_INCLUDES += -Igt -Ios -Imtk3_bsp2 -Imtk3_bsp2/config -Imtk3_bsp2/include -Imtk3_bsp2/mtkernel/kernel/knlinc
C_DEFS += -D_STM32CUBE_DISCOVERY_N657_

ASFLAGS = $(MCU) $(C_DEFS) $(C_INCLUDES) $(OPT) -Wall -fdata-sections -ffunction-sections
CFLAGS  = $(MCU) $(C_DEFS) $(C_INCLUDES) $(OPT) -Wall -fdata-sections -ffunction-sections -std=gnu11 -MMD -MP -MF"$(@:%.o=%.d)"

LDSCRIPT = STM32N657xx_guardian.ld
LIBS     = -lc -lm -lnosys -l:NetworkRuntime1200_CM55_GCC.a
LIBDIR   = -L$(ST_ROOT)/Middlewares/stedgeai-lib/Lib/GCC/ARMCortexM55
LDFLAGS  = -Wl,--wrap=tm_printf,--wrap=tm_putstring,--wrap=BSP_I2C1_Init,--wrap=BSP_I2C1_DeInit,--wrap=BSP_I2C1_ReadReg16,--wrap=BSP_I2C1_WriteReg16 $(MCU) -specs=nano.specs -T$(LDSCRIPT) $(LIBDIR) $(LIBS) -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref -Wl,--gc-sections \
           -u _printf_float -Wl,--print-memory-usage -Wl,--no-warn-rwx-segments

# object names: strip leading ../ so everything lands under build/
objname = $(BUILD_DIR)/$(subst ../,_up_/,$(basename $(1))).o
OBJECTS := $(foreach f,$(C_SOURCES) $(ASM_SOURCES) $(ASM_SOURCES_S),$(call objname,$(f)))

.PHONY: all clean sign
all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).bin

define c_rule
$(call objname,$(1)): $(1) Makefile
	@mkdir -p $$(dir $$@)
	@echo "  CC $(1)"
	@$$(CC) -c $$(CFLAGS) $(1) -o $$@
endef
define s_rule
$(call objname,$(1)): $(1) Makefile
	@mkdir -p $$(dir $$@)
	@echo "  AS $(1)"
	@$$(AS) -c $$(ASFLAGS) $(1) -o $$@
endef
$(foreach f,$(C_SOURCES),$(eval $(call c_rule,$(f))))
$(foreach f,$(ASM_SOURCES) $(ASM_SOURCES_S),$(eval $(call s_rule,$(f))))

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) $(LDSCRIPT)
	@echo "  LD $@"
	@$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	@$(SZ) $@

$(BUILD_DIR)/$(TARGET).bin: $(BUILD_DIR)/$(TARGET).elf
	@$(CP) -O binary $< $@

sign: $(BUILD_DIR)/$(TARGET).bin
	$(SIGNER) -s -bin $< -nk -t ssbl -hv 2.3 --align -o $(BUILD_DIR)/$(TARGET)_sign.bin

clean:
	rm -rf $(BUILD_DIR)

-include $(OBJECTS:.o=.d)
