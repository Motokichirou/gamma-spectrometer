/*
 * ring.h — кольцевой буфер байтов «один писатель — один читатель» (SPSC), без блокировок.
 * Переносимый: используется драйвером UART (приём из прерывания, передача по DMA), тестируется на ПК.
 *
 * Размер — степень двойки; индексы head/tail свободно бегущие (uint32, переполнение — норма),
 * поэтому вмещается ровно size байт (не size − 1).
 * Писатель меняет только head, читатель — только tail. Данные пишутся в buf ДО продвижения head
 * (buf и индексы volatile — компилятор порядок не переставит; ядро M4 одно, барьеры не нужны).
 */
#ifndef RING_H
#define RING_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    volatile uint8_t *buf;
    uint32_t mask;                 /* size − 1 */
    volatile uint32_t head;        /* пишет писатель */
    volatile uint32_t tail;        /* пишет читатель */
} ring_t;

/* size — степень двойки ≥ 2; иначе false и кольцо не годно */
bool     ring_init(ring_t *r, uint8_t *buf, uint32_t size);
uint32_t ring_size(const ring_t *r);
uint32_t ring_used(const ring_t *r);
uint32_t ring_free(const ring_t *r);
/* писатель: положить до n байт, вернуть сколько положено (остальное не влезло) */
uint32_t ring_put(ring_t *r, const uint8_t *p, uint32_t n);
bool     ring_put1(ring_t *r, uint8_t b);
/* читатель: забрать до cap байт */
uint32_t ring_get(ring_t *r, uint8_t *dst, uint32_t cap);
/* читатель: непрерывный кусок от хвоста (для DMA) и его отбрасывание после передачи */
uint32_t ring_peek_contig(const ring_t *r, const uint8_t **p);
void     ring_drop(ring_t *r, uint32_t n);

#endif
