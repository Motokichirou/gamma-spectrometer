#!/bin/sh
# Сборка gamma_fw. Тулчейн — из STM32CubeIDE 1.19.
# Падает при ошибке компиляции/линковки, при переполнении ОЗУ/CCM/флеш (ld) и если бюджет ОЗУ
# (статика core + резервы драйверов: DMA АЦП 32 КБ, кольцо UART 8 КБ, куча, стек 4 КБ) > 96 КБ.
set -e
cd "$(dirname "$0")"
TC=/c/ST/STM32CubeIDE_1.19.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.win32_1.0.0.202411081344/tools/bin
CM=../gamma_stage1/Drivers/CMSIS
F="-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard"
SRCS=$(find src -name '*.c' | sort)
$TC/arm-none-eabi-gcc $F -std=gnu11 -O2 -g -Wall -Wextra -fno-math-errno -ffunction-sections -fdata-sections \
  -DSTM32G474xx -Isrc/core -Isrc/hw -I$CM/Include -I$CM/Device/ST/STM32G4xx/Include \
  $SRCS startup_stm32g474xx.s -T gamma_fw.ld \
  --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections -Wl,--no-warn-rwx-segments \
  -Wl,-Map=gamma_fw.map -o gamma_fw.elf -lm
$TC/arm-none-eabi-objcopy -O binary gamma_fw.elf gamma_fw.bin
$TC/arm-none-eabi-size gamma_fw.elf

# ---- бюджет ОЗУ ----
RAM_MAX=98304
SYMS=$($TC/arm-none-eabi-nm -S --size-sort gamma_fw.elf)
symsize() { echo "$SYMS" | awk -v s="$1" '$4 == s { print strtonum("0x" $2) }'; }
need() {   # имя, ожидаемый размер — резерв драйвера должен попасть в образ (не выброшен gc)
  sz=$(symsize "$1")
  if [ "$sz" != "$2" ]; then echo "ОШИБКА: резерв $1 = '${sz}' байт, ожидалось $2" >&2; exit 1; fi
}
need adc_dma_buf 32768
need uart_tx_ring 8192
$TC/arm-none-eabi-size -A gamma_fw.elf | awk -v max=$RAM_MAX '
  $2 ~ /^[0-9]+$/ && $3 ~ /^[0-9]+$/ {
    if ($3 >= 536870912 && $3 < 536870912 + max) { ram += $2; printf "  ОЗУ  %-18s %6d\n", $1, $2 }
    if ($3 >= 268435456 && $3 < 268435456 + 32768) { ccm += $2; printf "  CCM  %-18s %6d\n", $1, $2 }
  }
  END {
    printf "ОЗУ: %d из %d байт (%.1f %%), свободно %d; CCM: %d из 32768\n", ram, max, 100.0 * ram / max, max - ram, ccm
    if (ram > max) { print "ОШИБКА: бюджет ОЗУ превышен" > "/dev/stderr"; exit 1 }
    if (ccm > 32768) { print "ОШИБКА: CCM переполнена" > "/dev/stderr"; exit 1 }
  }'
echo "  в том числе: DMA АЦП 32768, кольцо UART 8192, спектр $(symsize spec), куча+стек в ._user_heap_stack"
