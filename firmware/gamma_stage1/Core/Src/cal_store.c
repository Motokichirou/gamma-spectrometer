/* cal_store.c — хранение энергокалибровки во внутренней flash STM32G474.
 *
 * Блок в последней странице 2 КБ. G474RE по умолчанию dual-bank (DBANK=1):
 * Bank2, page 127 → адрес 0x0807F800. Запись редкая (по команде -wcal),
 * износ не критичен. Переопределяет слабые хуки app_cal_write/app_cal_read.
 */
#include "main.h"        /* HAL: FLASH_*, HAL_FLASH* */
#include "app.h"
#include "shproto.h"     /* shproto_crc16 для контроля целостности */
#include <string.h>
#include <stddef.h>

#define CAL_ADDR   0x0807F800UL   /* последняя страница 2КБ (dual-bank) */
#define CAL_MAGIC  0x47434131UL   /* 'GCA1' */

typedef struct {
    uint32_t magic;
    uint32_t order;       /* степень полинома (1..4) */
    double   coef[5];     /* c0..c4: E = Σ cᵢ·chⁱ */
    uint32_t crc;         /* shproto_crc16 по magic..coef */
    uint32_t pad;         /* до кратности 8 байт (7 doubleword) */
} cal_block_t;

int app_cal_write(int order, const double *coef)
{
    if (order < 1 || order > 4) return 0;

    cal_block_t b;
    memset(&b, 0, sizeof b);
    b.magic = CAL_MAGIC;
    b.order = (uint32_t)order;
    for (int i = 0; i < 5; i++) b.coef[i] = (i <= order) ? coef[i] : 0.0;
    b.crc = shproto_crc16((const uint8_t *)&b, offsetof(cal_block_t, crc));

    HAL_FLASH_Unlock();
    FLASH_EraseInitTypeDef e = {0};
    e.TypeErase = FLASH_TYPEERASE_PAGES;
    e.Banks     = FLASH_BANK_2;
    e.Page      = 127;
    e.NbPages   = 1;
    uint32_t perr = 0;
    int ok = (HAL_FLASHEx_Erase(&e, &perr) == HAL_OK);
    if (ok) {
        const uint64_t *src = (const uint64_t *)&b;
        for (uint32_t i = 0; i * 8u < sizeof b; i++) {
            if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                                  CAL_ADDR + i * 8u, src[i]) != HAL_OK) {
                ok = 0; break;
            }
        }
    }
    HAL_FLASH_Lock();
    return ok;
}

int app_cal_read(int *order, double *coef)
{
    const cal_block_t *b = (const cal_block_t *)CAL_ADDR;
    if (b->magic != CAL_MAGIC) return 0;
    if ((uint16_t)b->crc != shproto_crc16((const uint8_t *)b, offsetof(cal_block_t, crc)))
        return 0;
    if (order) *order = (int)b->order;
    if (coef) for (int i = 0; i < 5; i++) coef[i] = b->coef[i];
    return 1;
}
