/* spiflash.h — W25Q64 (U303) в глубокий сон (SPEC §5, §7.9: никаких переключаемых нагрузок
 * на +3.3V_D при включённом ВВ — флеш спит всё время работы прибора).
 *
 * Выводы (сверено по pcb/gamma_spectrometer.net): PA15 (39) = /CS, PB3 (40) = CLK,
 * PB4 (41) = DO/MISO, PB5 (42) = DI/MOSI; /WP и /HOLD — на +3.3V_D.
 */
#ifndef SPIFLASH_H
#define SPIFLASH_H

/* Команда Power-down 0xB9 (режим SPI 0, программно — периферия SPI1 не включается), затем
   CLK/MOSI/MISO — в аналог, /CS — выход 1. Повторный вызов безвреден (0xB9 спящей флеш
   игнорируется до Release 0xAB). Звать после clock_init (задержки по DWT), до включения ВВ. */
void spiflash_sleep(void);

#endif
