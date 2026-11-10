/* flash_int.c — см. flash_int.h. Порядок операций — RM0440 §3.3.7 (стирание страницы),
   §3.3.8 (запись двойного слова); логика «стереть → записать → сверить» — core/flashstore. */
#include "stm32g474xx.h"
#include "flash_int.h"
#include "flashstore.h"
#include "timebase.h"

#define FLASH_KB        (*(volatile const uint16_t *)FLASHSIZE_BASE)
#define KEY1            0x45670123u
#define KEY2            0xCDEF89ABu
#define SR_ERR          (FLASH_SR_OPERR | FLASH_SR_PROGERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR | \
                         FLASH_SR_SIZERR | FLASH_SR_PGSERR | FLASH_SR_MISERR | FLASH_SR_FASTERR | \
                         FLASH_SR_RDERR | FLASH_SR_OPTVERR)
#define ERASE_TMO_MS    100u                  /* стирание страницы: тип. 22 мс, макс. 40 мс */
#define PROG_TMO_MS     2u                    /* двойное слово: макс. ~90 мкс */

extern uint32_t SystemCoreClock;

bool flash_layout_ok(void)
{
    return (FLASH->OPTR & FLASH_OPTR_DBANK) == 0u && FLASH_KB >= 128u;
}

/* ждать конца операции; true — без ошибок. Пока стирается страница, ЦП стоит на выборке
   команд из флеш, так что цикл обычно проходит один раз. */
static bool wait_done(uint32_t tmo_ms)
{
    uint32_t t0 = cycles(), lim = (SystemCoreClock / 1000u) * tmo_ms;
    while (FLASH->SR & FLASH_SR_BSY)
        if (cycles() - t0 > lim) return false;
    uint32_t sr = FLASH->SR;
    FLASH->SR = sr & (SR_ERR | FLASH_SR_EOP);   /* rc_w1 */
    return (sr & SR_ERR) == 0u;
}

/* после стирания/записи — сбросить кэши команд и данных (RM0440 §3.3.3, как HAL FLASH_FlushCaches) */
static void flush_caches(void)
{
    uint32_t acr = FLASH->ACR;
    if (acr & FLASH_ACR_ICEN) {
        FLASH->ACR &= ~FLASH_ACR_ICEN;
        FLASH->ACR |= FLASH_ACR_ICRST;
        FLASH->ACR &= ~FLASH_ACR_ICRST;
        FLASH->ACR |= FLASH_ACR_ICEN;
    }
    if (acr & FLASH_ACR_DCEN) {
        FLASH->ACR &= ~FLASH_ACR_DCEN;
        FLASH->ACR |= FLASH_ACR_DCRST;
        FLASH->ACR &= ~FLASH_ACR_DCRST;
        FLASH->ACR |= FLASH_ACR_DCEN;
    }
}

static bool unlock(void)
{
    if (!wait_done(ERASE_TMO_MS)) {             /* чужая операция или старые флаги ошибок */
        if (FLASH->SR & FLASH_SR_BSY) return false;
    }
    FLASH->SR = SR_ERR | FLASH_SR_EOP;
    if (FLASH->CR & FLASH_CR_LOCK) {
        FLASH->KEYR = KEY1;
        FLASH->KEYR = KEY2;
    }
    return (FLASH->CR & FLASH_CR_LOCK) == 0u;
}

static void lock(void)
{
    FLASH->CR &= ~(FLASH_CR_PG | FLASH_CR_PER | FLASH_CR_PNB | FLASH_CR_BKER);
    FLASH->CR |= FLASH_CR_LOCK;
}

/* ---- операции для core/flashstore ---- */
static bool op_erase(void *ctx)
{
    (void)ctx;
    FLASH->CR = (FLASH->CR & ~(FLASH_CR_PG | FLASH_CR_PNB | FLASH_CR_BKER))
              | FLASH_CR_PER | (FLASH_SETTINGS_PAGE << FLASH_CR_PNB_Pos);
    FLASH->CR |= FLASH_CR_STRT;
    bool ok = wait_done(ERASE_TMO_MS);
    FLASH->CR &= ~(FLASH_CR_PER | FLASH_CR_PNB);
    flush_caches();
    return ok;
}

static bool op_prog64(void *ctx, uint32_t off, uint32_t lo, uint32_t hi)
{
    (void)ctx;
    volatile uint32_t *dst = (volatile uint32_t *)(FLASH_SETTINGS_ADDR + off);
    FLASH->CR |= FLASH_CR_PG;
    uint32_t pm = __get_PRIMASK();
    __disable_irq();                            /* два слова подряд, без вклинивания */
    dst[0] = lo;
    __ISB();
    dst[1] = hi;
    __set_PRIMASK(pm);
    bool ok = wait_done(PROG_TMO_MS);
    FLASH->CR &= ~FLASH_CR_PG;
    return ok;
}

static const uint8_t *op_page(void *ctx)
{
    (void)ctx;
    return (const uint8_t *)FLASH_SETTINGS_ADDR;
}

const uint8_t *flash_settings_image(void)
{
    return flash_layout_ok() ? (const uint8_t *)FLASH_SETTINGS_ADDR : NULL;
}

bool flash_settings_load(settings_t *s)
{
    if (!flash_layout_ok()) return false;       /* при DBANK = 1 адрес — дыра: чтение дало бы сбой шины */
    return fstore_load((const uint8_t *)FLASH_SETTINGS_ADDR, s);
}

bool flash_settings_store(const settings_t *s)
{
    if (!flash_layout_ok()) return false;
    if (!unlock()) { lock(); return false; }
    const fstore_ops_t ops = { op_erase, op_prog64, op_page, 0 };
    bool ok = fstore_save(s, &ops);
    lock();
    flush_caches();
    return ok;
}
