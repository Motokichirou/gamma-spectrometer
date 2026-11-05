/*
 * shproto.h — протокол AtomSpectra/BecqMoni (shproto)
 *
 * Кадр:  0xFF  0xFE  [CMD][PAYLOAD...][CRC_LSB][CRC_MSB]  0xA5
 *   0xFF — keepalive-префикс (игнорируется парсером)
 *   0xFE — START (сброс парсера)
 *   0xFD — ESC: следующий байт передан инвертированным (~byte)
 *   0xA5 — FINISH
 * Стаффингу подлежат 0xFE, 0xFD, 0xA5 внутри кадра (включая байты CRC).
 * CRC16 MODBUS (init 0xFFFF, poly 0xA001) считается по нестаффленным CMD+PAYLOAD.
 *
 * Модуль переносимый: без HAL, без malloc. Тестируется на ПК.
 */
#ifndef SHPROTO_H
#define SHPROTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SHPROTO_KEEPALIVE  0xFFu
#define SHPROTO_START      0xFEu
#define SHPROTO_ESC        0xFDu
#define SHPROTO_FINISH     0xA5u

/* Команды (режимы) */
#define SHPROTO_CMD_HISTOGRAM  0x01u  /* устройство → ПК: спектр */
#define SHPROTO_CMD_PULSE      0x02u  /* устройство → ПК: форма импульса */
#define SHPROTO_CMD_TEXT       0x03u  /* оба направления: ASCII-команды/ответы */
#define SHPROTO_CMD_STAT       0x04u  /* устройство → ПК: статус (триггер отрисовки BecqMoni!) */

/* Максимальный принимаемый payload (команды ПК короткие — текст) */
#define SHPROTO_RX_MAX_PAYLOAD  256u

typedef struct {
    uint8_t  state;     /* 0=IDLE, 1=ждём CMD, 2=DATA */
    bool     esc;
    uint8_t  cmd;
    uint8_t  buf[SHPROTO_RX_MAX_PAYLOAD + 2]; /* payload + 2 байта CRC */
    uint16_t len;
} shproto_rx_t;

/* CRC16 MODBUS */
uint16_t shproto_crc16(const uint8_t *data, size_t n);
uint16_t shproto_crc16_update(uint16_t crc, uint8_t byte);

/* Приём: скормить один байт. Возвращает true, когда принят валидный кадр —
 * тогда rx->cmd и rx->buf[0..rx->len-1] содержат команду и payload (CRC уже снят). */
void shproto_rx_init(shproto_rx_t *rx);
bool shproto_rx_byte(shproto_rx_t *rx, uint8_t byte);

/* Передача: собрать кадр (префикс 0xFF + START + стаффинг + CRC + FINISH).
 * out должен вмещать 2*(len+3) + 3 байта (худший случай стаффинга).
 * Возвращает длину собранного кадра. */
size_t shproto_build(uint8_t cmd, const uint8_t *payload, size_t len, uint8_t *out);

#endif /* SHPROTO_H */
