/* spi1.h — SPI1 под флешку W25Q64 (U303) для регистратора: PB3 = SCK, PB4 = MISO, PB5 = MOSI (AF5),
 * PA15 = /CS (GPIO). Режим 0, 8 бит, PCLK2 156 МГц / 8 = 19.5 МГц, опрос без DMA
 * (256 байт ≈ 0.15 мс — укладывается в шаг регистратора, SPEC_logger.md §2.1).
 * Звать после spiflash_sleep(): тот оставляет выводы аналоговыми, здесь они переходят в AF. */
#ifndef SPI1_H
#define SPI1_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "w25.h"

void spi1_init(void);
extern const w25_port_t spi1_w25;

#endif
