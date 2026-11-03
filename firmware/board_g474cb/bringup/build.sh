#!/bin/sh
# Сборка прошивки запуска платы. Тулчейн — из STM32CubeIDE 1.19.
set -e
cd "$(dirname "$0")"
TC=/c/ST/STM32CubeIDE_1.19.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.win32_1.0.0.202411081344/tools/bin
CM=../../gamma_stage1/Drivers/CMSIS
F="-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard"
$TC/arm-none-eabi-gcc $F -O2 -g -Wall -Wextra -DSTM32G474xx -I$CM/Include -I$CM/Device/ST/STM32G4xx/Include \
  main.c startup_stm32g474xx.s -T ../STM32G474CBTX_FLASH.ld \
  --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections -Wl,-Map=bringup.map -o bringup.elf
$TC/arm-none-eabi-objcopy -O binary bringup.elf bringup.bin
$TC/arm-none-eabi-size bringup.elf
