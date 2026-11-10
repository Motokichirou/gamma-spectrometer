/*
 * w25.h — команды SPI-флешки W25Q64JV поверх абстрактного порта (железо — hw/spi1.c, на ПК —
 * модель NOR-флешки в tests/). Ничего не ждёт: стирание и запись только запускаются,
 * готовность — опросом w25_busy() (SPEC_logger.md §2.1).
 */
#ifndef W25_H
#define W25_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define W25_PAGE        256u
#define W25_SECTOR      4096u
#define W25_BLOCK       65536u
#define W25_SIZE        (8u * 1024u * 1024u)
#define W25_JEDEC_MFR   0xEFu
#define W25_JEDEC_TYPE  0x40u
#define W25_JEDEC_CAP   0x17u           /* 64 Мбит */

typedef struct {
    void (*select)(bool on);                                 /* /CS: true — выбрать (низкий) */
    void (*xfer)(const uint8_t *tx, uint8_t *rx, size_t n);  /* tx NULL — слать 0xFF; rx NULL — не читать */
    void (*delay_us)(uint32_t us);
} w25_port_t;

void w25_wake(const w25_port_t *p);                          /* 0xAB + tRES1 */
void w25_sleep(const w25_port_t *p);                         /* 0xB9 */
void w25_jedec(const w25_port_t *p, uint8_t id[3]);          /* 0x9F */
bool w25_busy(const w25_port_t *p);                          /* RDSR1 бит 0 */
void w25_read(const w25_port_t *p, uint32_t addr, void *buf, size_t n);         /* 0x03 */
/* запись внутри одной страницы (адрес + n не пересекает границу 256 Б) — только запуск */
void w25_program(const w25_port_t *p, uint32_t addr, const void *buf, size_t n);
void w25_erase4k(const w25_port_t *p, uint32_t addr);        /* 0x20 — только запуск */
void w25_erase64k(const w25_port_t *p, uint32_t addr);       /* 0xD8, блок 64 КБ (тип. 150 мс) — только запуск */

#endif
