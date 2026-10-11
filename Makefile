# Builds BareMetal.elf/.bin without CubeIDE. Needs arm-none-eabi-gcc on PATH.
#   make            build (unoptimised with debug info, same as the IDE's Debug config)
#   make OPT=-O2    build optimised
#   make size       show flash/RAM use
#   make clean

TARGET  := BareMetal
BUILD   := build
OPT     ?= -O0 -g3
CC      := arm-none-eabi-gcc
OBJCOPY := arm-none-eabi-objcopy
SIZE    := arm-none-eabi-size

SRCS := $(filter-out Core/Src/stm32f4xx_hal_msp.c, $(wildcard Core/Src/*.c)) \
        $(wildcard BareMetalDrivers/Src/*.c) \
        Core/Startup/startup_stm32f446retx.s
OBJS := $(addprefix $(BUILD)/, $(addsuffix .o, $(basename $(SRCS))))

CPU    := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
INCS   := -ICore/Inc -IBareMetalDrivers/Inc \
          -IDrivers/CMSIS/Device/ST/STM32F4xx/Include -IDrivers/CMSIS/Include
CFLAGS := $(CPU) -std=gnu11 -DSTM32F446xx $(INCS) $(OPT) -Wall \
          -ffunction-sections -fdata-sections -MMD -MP
ASFLAGS := $(CPU) -x assembler-with-cpp
LDFLAGS := $(CPU) -TSTM32F446RETX_FLASH.ld --specs=nano.specs --specs=nosys.specs \
           -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map -static -Wl,--start-group -lc -lm -Wl,--end-group

all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin size

$(BUILD)/$(TARGET).elf: $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

size: $(BUILD)/$(TARGET).elf
	@$(SIZE) $<

clean:
	rm -rf $(BUILD)

.PHONY: all size clean
-include $(OBJS:.o=.d)
