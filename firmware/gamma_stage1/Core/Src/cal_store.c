/* cal_store.c — хранение энергокалибровки во внутренней flash STM32G474.
 *
 * Формат хранения — 11 «сырых» 32-битных слов, как их шлёт BecqMoni командой
 * `-cal <i> <hex>` (i=0..10):
 *   слова 0-9 = пять коэффициентов c0..c4 (IEEE-754 double), каждый двумя
 *               словами: чётный индекс = старшее слово, нечётный = младшее;
 *   слово 10  = контрольная сумма BecqMoni (алгоритм не воспроизводим — хранится
 *               и отдаётся как есть, чтобы write→read-back сверка совпала).
 * Храним слова дословно ⇒ чтение прибором байт-в-байт равно записанному.
 *
 * Блок — в последней странице флеш. Размер флеш читаем из заводского регистра
 * (RM0440 §48.2), а не прошиваем константой: один бинарник годится и для
 * 512 КБ (G474ME, Nucleo-G474RE), и для 256 КБ (G474MC). При DBANK=1 (заводское
 * значение у G474) страница 2 КБ и последняя лежит во втором банке; при DBANK=0
 * страница 4 КБ и банк один. Запись редкая, износ не критичен.
 * Переопределяет слабые хуки app_cal_*.
 */
#include "main.h"        /* HAL: FLASH_*, HAL_FLASH* */
#include "app.h"
#include "shproto.h"     /* shproto_crc16 для контроля целостности блока */
#include <string.h>
#include <stddef.h>

#define CAL_MAGIC  0x47434132UL   /* 'GCA2' (v2: 11 сырых слов) */

/* Размер флеш в КБ, заводская константа (RM0440 §48.2): 512 у ME, 256 у MC. */
#define FLASH_KB   (*(volatile const uint16_t *)0x1FFF75E0UL)

static uint32_t page_size(void)
{
    return (FLASH->OPTR & FLASH_OPTR_DBANK) ? 2048u : 4096u;
}

/* Адрес блока калибровки = начало последней страницы флеш.
 * 512 КБ + DBANK=1 → 0x0807F800 (как было константой); 256 КБ → 0x0803F800. */
static uint32_t cal_addr(void)
{
    return FLASH_BASE + (uint32_t)FLASH_KB * 1024u - page_size();
}

typedef struct {
    uint32_t magic;
    uint32_t word[11];    /* как от BecqMoni: -cal 0..10 */
    uint32_t crc;         /* shproto_crc16 по magic..word[10] */
    uint32_t pad;         /* до кратности 8 байт (7 doubleword = 56 байт) */
} cal_block_t;

/* --- накопитель слов в RAM: BecqMoni шлёт 11 команд по одной --------------- */
static uint32_t stage[11];
static uint16_t stage_mask;   /* бит i = слово i получено */

static int flash_erase_page(void)
{
    FLASH_EraseInitTypeDef e = {0};
    e.TypeErase = FLASH_TYPEERASE_PAGES;
    /* Страниц в банке = КБ/4 при обеих раскладках (2 КБ × 2 банка либо 4 КБ × 1),
     * поэтому номер последней страницы считается одинаково. */
    e.Banks     = (FLASH->OPTR & FLASH_OPTR_DBANK) ? FLASH_BANK_2 : FLASH_BANK_1;
    e.Page      = (uint32_t)FLASH_KB / 4u - 1u;
    e.NbPages   = 1;
    uint32_t perr = 0;
    return (HAL_FLASHEx_Erase(&e, &perr) == HAL_OK);
}

static int flash_write_block(const cal_block_t *b)
{
    HAL_FLASH_Unlock();
    int ok = flash_erase_page();
    if (ok) {
        const uint64_t *src = (const uint64_t *)b;
        const uint32_t addr = cal_addr();
        for (uint32_t i = 0; i * 8u < sizeof *b; i++) {
            if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                                  addr + i * 8u, src[i]) != HAL_OK) {
                ok = 0; break;
            }
        }
    }
    HAL_FLASH_Lock();
    return ok;
}

static int read_words(uint32_t w[11])
{
    const cal_block_t *b = (const cal_block_t *)cal_addr();
    if (b->magic != CAL_MAGIC) return 0;
    if ((uint16_t)b->crc != shproto_crc16((const uint8_t *)b, offsetof(cal_block_t, crc)))
        return 0;
    for (int i = 0; i < 11; i++) w[i] = b->word[i];
    return 1;
}

static int commit_words(const uint32_t w[11])
{
    cal_block_t b;
    memset(&b, 0, sizeof b);
    b.magic = CAL_MAGIC;
    for (int i = 0; i < 11; i++) b.word[i] = w[i];
    b.crc = shproto_crc16((const uint8_t *)&b, offsetof(cal_block_t, crc));
    return flash_write_block(&b);
}

/* --- хуки для BecqMoni-протокола (app.c) ----------------------------------- */

/* Приём одного слова `-cal <i> <hex>`. Копим в RAM; когда пришли все 11 —
 * один раз шьём страницу. Возврат: 1 = слово принято. */
int app_cal_stage_word(int idx, uint32_t word)
{
    if (idx < 0 || idx > 10) return 0;
    stage[idx] = word;
    stage_mask |= (uint16_t)(1u << idx);
    if (stage_mask == 0x7FFu) {        /* все 11 получены — фиксируем */
        commit_words(stage);
        stage_mask = 0;
    }
    return 1;
}

/* Чтение 11 слов (для отдачи BecqMoni на голый `-cal` и на `-cal <i>`). */
int app_cal_read_words(uint32_t w[11])
{
    return read_words(w);
}

/* Стереть калибровку (страница → 0xFF, magic невалиден ⇒ "CAL: none"). */
int app_cal_clear(void)
{
    stage_mask = 0;
    HAL_FLASH_Unlock();
    int ok = flash_erase_page();
    HAL_FLASH_Lock();
    return ok;
}

/* --- хуки для нашего сервисного тула (order + double-коэффициенты) ---------- */

/* Контрольная сумма BecqMoni для слова 10: стандартный CRC-32 (zip/ISO-HDLC,
 * poly 0x04C11DB7 reflected = 0xEDB88320, init/xorout 0xFFFFFFFF) по ASCII-строке
 * из слов 0..9, каждое как 8-значный hex в ВЕРХНЕМ регистре, без разделителей.
 * Воспроизведено реверсом двух записей реального BecqMoni. */
static uint32_t becq_checksum(const uint32_t w[10])
{
    char s[81];
    for (int k = 0; k < 10; k++)
        snprintf(s + 8 * k, 9, "%08lX", (unsigned long)w[k]);
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < 80; i++) {
        crc ^= (uint8_t)s[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

static double u64_to_double(uint64_t u)
{
    double d;
    memcpy(&d, &u, sizeof d);
    return d;
}
static uint64_t double_to_u64(double d)
{
    uint64_t u;
    memcpy(&u, &d, sizeof u);
    return u;
}

int app_cal_write(int order, const double *coef)
{
    if (order < 1 || order > 4) return 0;
    uint32_t w[11];
    for (int k = 0; k < 5; k++) {
        double c = (k <= order) ? coef[k] : 0.0;
        uint64_t u = double_to_u64(c);
        w[2 * k]     = (uint32_t)(u >> 32);   /* старшее слово */
        w[2 * k + 1] = (uint32_t)(u & 0xFFFFFFFFu);
    }
    w[10] = becq_checksum(w);   /* настоящая сумма ⇒ BecqMoni примет при чтении */
    return commit_words(w);
}

int app_cal_read(int *order, double *coef)
{
    uint32_t w[11];
    if (!read_words(w)) return 0;
    double c[5];
    int ord = 0;
    for (int k = 0; k < 5; k++) {
        uint64_t u = ((uint64_t)w[2 * k] << 32) | w[2 * k + 1];
        c[k] = u64_to_double(u);
        if (c[k] != 0.0) ord = k;   /* старший ненулевой = степень */
    }
    if (ord < 1) ord = 1;
    if (order) *order = ord;
    if (coef) for (int k = 0; k < 5; k++) coef[k] = c[k];
    return 1;
}
