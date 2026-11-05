/* link.ld — раскладка обвязки для эмулятора (tools/cyc): флеш, CCM (как в прошивке), ОЗУ с запасом.
   Сегменты грузятся по адресам исполнения (cyc.py), стартового кода нет. */
MEMORY {
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
  CCM   (rwx) : ORIGIN = 0x10000000, LENGTH = 32K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 1M
}
SECTIONS {
  .text    : { *(.text*) *(.rodata*) *(.glue_7*) } > FLASH
  .ARM.exidx : { *(.ARM.exidx*) } > FLASH
  .ccmfunc : { *(.ccmfunc*) } > CCM
  .ccmdata : { *(.ccmdata*) } > CCM
  .ccmbss (NOLOAD) : { *(.ccmbss*) } > CCM
  .data    : { *(.data*) } > RAM
  .bss (NOLOAD) : { *(.bss*) *(COMMON) } > RAM
}
