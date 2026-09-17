# CLI build for the TRON μT-Kernel camera example (FSBL project). Mirrors the CubeIDE flags;
# the generated Debug/makefile carries Windows absolute paths and cannot be used here.
#   make -f gt.mk -j8      -> build/prj_stm32n6_cam_FSBL.bin   (load at 0x34180400)
OUT := build
TARGET := $(OUT)/prj_stm32n6_cam_FSBL
CC := arm-none-eabi-gcc
OBJCOPY := arm-none-eabi-objcopy

# Object list taken from Debug/objects.list minus the #if-guarded other-CPU files.
OBJS_REL := $(shell tr -d '"\r' < Debug/objects.list | sed 's#^\./##' | \
  grep -vE 'rx231|rx65n|rza2m|stm32h7|stm32l4|tx03_m367|armv7a|rxv2|iote_|nxp_mcux|ra_fsp|xmc_mtb|nucleo_|no_device|armv7m|stm32f4|stm32f7|stm32g4')
OBJS := $(addprefix $(OUT)/,$(OBJS_REL))

INCS := -ICore/Inc -IDrivers/BSP/STM32N6570-DK -IDrivers/STM32N6xx_HAL_Driver/Inc \
  -IDrivers/CMSIS/Device/STM32N6xx/Include -IDrivers/STM32N6xx_HAL_Driver/Inc/Legacy -IDrivers/CMSIS/Include \
  -IDrivers/BSP/Components/IMX335 -IDrivers/BSP/Components/Common -IDrivers/BSP/Components/rk050hr18 \
  -IMiddlewares/STM32_ISP/evision/inc -IMiddlewares/STM32_ISP/inc \
  -Imtk3_bsp2 -Imtk3_bsp2/config -Imtk3_bsp2/include -Imtk3_bsp2/mtkernel/kernel/knlinc
MCU := -mcpu=cortex-m55 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb
DEFS := -DSTM32N657xx -DUSE_FULL_ASSERT -DUSE_HAL_DRIVER -D_STM32CUBE_DISCOVERY_N657_
CFLAGS := $(MCU) -std=gnu11 -g3 $(DEFS) $(INCS) -O0 -ffunction-sections -fdata-sections -Wall -mcmse --specs=nano.specs
ASFLAGS := $(MCU) -g3 -D_STM32CUBE_DISCOVERY_N657_ $(INCS) -x assembler-with-cpp --specs=nano.specs
LDFLAGS := $(MCU) -T STM32N657X0HXQ_AXISRAM2_fsbl.ld --specs=nosys.specs --specs=nano.specs \
  -Wl,-Map=$(TARGET).map -Wl,--gc-sections -static -Wl,--cmse-implib -Wl,--out-implib=$(OUT)/implib.o \
  -LMiddlewares/STM32_ISP/evision
LIBS := -l:libn6-evision-awb_gcc.a -l:libn6-evision-st-ae_gcc.a -Wl,--start-group -lc -lm -Wl,--end-group

all: $(TARGET).bin
$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	arm-none-eabi-size $<
$(TARGET).elf: $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS) $(LIBS)
$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) $< -o $@
$(OUT)/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) -c $(ASFLAGS) $< -o $@
$(OUT)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) -c $(ASFLAGS) $< -o $@
clean:
	rm -rf $(OUT)
.PHONY: all clean
