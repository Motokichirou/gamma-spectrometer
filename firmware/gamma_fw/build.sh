#!/bin/sh
# Сборка gamma_fw. Тулчейн — из STM32CubeIDE 1.19.
set -e
cd "$(dirname "$0")"
TC=/c/ST/STM32CubeIDE_1.19.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.win32_1.0.0.202411081344/tools/bin
CM=../gamma_stage1/Drivers/CMSIS
F="-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard"
SRCS=$(find src -name '*.c' | sort)
$TC/arm-none-eabi-gcc $F -std=gnu11 -O2 -g -Wall -Wextra -ffunction-sections -fdata-sections \
  -DSTM32G474xx -Isrc/core -I$CM/Include -I$CM/Device/ST/STM32G4xx/Include \
  $SRCS startup_stm32g474xx.s -T gamma_fw.ld \
  --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections -Wl,-Map=gamma_fw.map -o gamma_fw.elf
$TC/arm-none-eabi-objcopy -O binary gamma_fw.elf gamma_fw.bin
$TC/arm-none-eabi-size gamma_fw.elf
