/* Прошивка первого запуска боевой платы МК (STM32G474CBT6, LQFP-48).
 * Без HAL, на регистрах CMSIS. Прошивается по UART через системный загрузчик.
 *
 * Делает:
 *   - сразу прижимает ВВ: PB10 (HV_ENABLE) = 0, PA8 (HV_CONTROL) = 0;
 *   - USART1 PA9/PA10, 115200 8N1 (через FT231X на USB-плате);
 *   - печатает DEV_ID, объём флеш, корпус, FLASH_OPTR;
 *   - запускает HSE (кварц 16 МГц) и переводит на него SYSCLK —
 *     если после перехода текст читается, кварц на 16 МГц исправен;
 *   - раз в секунду: статус (VREF+, ВВ); D102 = индикатор ВВ;
 *   - ВВ: ШИМ PA8 31 кГц, монитор PA0, подстройка, защита; команды hv/hvzero/a/n/d (см. help);
 *   - по строке "!boot" прыгает в системный загрузчик (для следующих прошивок без BOOT0).
 */
#include <stdint.h>
#include "stm32g4xx.h"

#define BAUD 115200u
#define VREFINT_CAL (*(const uint16_t *)0x1FFF75AAu) /* при VREF+ = 3.0 В, 30 °C */
#define FLASH_KB    (*(const uint16_t *)0x1FFF75E0u)
#define PKG_ID      (*(const uint16_t *)0x1FFF7500u)

static volatile uint32_t ms;
void SysTick_Handler(void) { ms++; }

void SystemInit(void)
{
    SCB->CPACR |= (3u << 20) | (3u << 22); /* FPU */
    /* Таблица векторов — наша. После команды Go загрузчика VTOR может смотреть в его ПЗУ,
     * и первый же SysTick улетит в чужой обработчик. */
    SCB->VTOR = 0x08000000u;
    __DSB();
}

static uint32_t sysclk = 16000000u;

static void uart_putc(char c)
{
    while (!(USART1->ISR & USART_ISR_TXE_TXFNF)) {}
    USART1->TDR = (uint8_t)c;
}
static void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}
static void uart_hex(uint32_t v, int nd)
{
    static const char h[] = "0123456789ABCDEF";
    for (int i = nd - 1; i >= 0; i--) uart_putc(h[(v >> (4 * i)) & 15]);
}
static void uart_dec(uint32_t v)
{
    char b[11]; int i = 0;
    do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) uart_putc(b[--i]);
}
static void uart_flush(void) { while (!(USART1->ISR & USART_ISR_TC)) {} }

static void uart_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    (void)RCC->APB2ENR;
    USART1->CR1 = 0;
    USART1->BRR = (sysclk + BAUD / 2) / BAUD;
    USART1->CR1 = USART_CR1_FIFOEN | USART_CR1_TE | USART_CR1_RE | USART_CR1_UE; /* FIFO 8 байт */
}

static void gpio_init(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN;
    (void)RCC->AHB2ENR;

    /* ВВ выключено: PB10 = 0, PA8 = 0, оба выходы push-pull */
    GPIOB->BSRR = 1u << (10 + 16);
    GPIOB->MODER = (GPIOB->MODER & ~(3u << 20)) | (1u << 20);
    GPIOA->BSRR = 1u << (8 + 16);
    GPIOA->MODER = (GPIOA->MODER & ~(3u << 16)) | (1u << 16);

    /* LED D102: PB6 выход */
    GPIOB->BSRR = 1u << (6 + 16);
    GPIOB->MODER = (GPIOB->MODER & ~(3u << 12)) | (1u << 12);

    /* USART1: PA9 TX, PA10 RX, AF7, подтяжка на RX */
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~((15u << 4) | (15u << 8))) | (7u << 4) | (7u << 8);
    GPIOA->PUPDR = (GPIOA->PUPDR & ~(3u << 20)) | (1u << 20);
    GPIOA->MODER = (GPIOA->MODER & ~((3u << 18) | (3u << 20))) | (2u << 18) | (2u << 20);
}

static int hse_start(void)
{
    RCC->CR |= RCC_CR_HSEON;
    uint32_t t = ms;
    while (!(RCC->CR & RCC_CR_HSERDY)) {
        if (ms - t > 200) { RCC->CR &= ~RCC_CR_HSEON; return 0; }
    }
    return 1;
}

static void sysclk_to_hse(void)
{
    uart_flush();
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | (2u << RCC_CFGR_SW_Pos);
    while (((RCC->CFGR & RCC_CFGR_SWS) >> RCC_CFGR_SWS_Pos) != 2u) {}
    /* HSE = 16 МГц, как HSI: делители UART и SysTick не меняются */
}

/* SYSCLK = PLL 128 МГц от 16 МГц (HSE или HSI): M=1, N=16 -> VCO 256, R=2.
 * Нужно ради ШИМ HV_CONTROL: 128 МГц / 4096 = 31 кГц вместо 3.9 кГц (слышимый писк X7R). */
static void sysclk_to_pll(int from_hse)
{
    uart_flush();
    FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | FLASH_ACR_LATENCY_4WS;
    while ((FLASH->ACR & FLASH_ACR_LATENCY) != FLASH_ACR_LATENCY_4WS) {}
    RCC->PLLCFGR = (from_hse ? 3u : 2u) | (0u << RCC_PLLCFGR_PLLM_Pos) |
                   (16u << RCC_PLLCFGR_PLLN_Pos) | (0u << RCC_PLLCFGR_PLLR_Pos) | RCC_PLLCFGR_PLLREN;
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) {}
    /* >80 МГц: переключаться через AHB/2 (RM0440 §7.2.7) */
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_HPRE) | RCC_CFGR_HPRE_DIV2;
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | (3u << RCC_CFGR_SW_Pos);
    while (((RCC->CFGR & RCC_CFGR_SWS) >> RCC_CFGR_SWS_Pos) != 3u) {}
    for (volatile int i = 0; i < 200; i++) {}
    RCC->CFGR &= ~RCC_CFGR_HPRE;
    sysclk = 128000000u;
    SysTick_Config(sysclk / 1000u);
    uart_init();
}

static void adc_on(ADC_TypeDef *a)
{
    a->CR &= ~ADC_CR_DEEPPWD;
    a->CR |= ADC_CR_ADVREGEN;
    uint32_t t = ms; while (ms - t < 2) {}
    a->CR |= ADC_CR_ADCAL;
    while (a->CR & ADC_CR_ADCAL) {}
    t = ms; while (ms - t < 1) {}
    a->ISR = ADC_ISR_ADRDY;
    a->CR |= ADC_CR_ADEN;
    while (!(a->ISR & ADC_ISR_ADRDY)) {}
    a->SMPR1 = 0x3FFFFFFFu;  /* все каналы: 640.5 такта */
    a->SMPR2 = 0x07FFFFFFu;
}

static void adc_init(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_ADC12EN;
    (void)RCC->AHB2ENR;
    ADC12_COMMON->CCR = (3u << ADC_CCR_CKMODE_Pos) | ADC_CCR_VREFEN | ADC_CCR_VBATSEL; /* HCLK/4 = 32 МГц */
    adc_on(ADC1);
    adc_on(ADC2);
}

static uint32_t adc_read(ADC_TypeDef *a, uint32_t ch)
{
    a->SQR1 = ch << ADC_SQR1_SQ1_Pos;   /* L = 0 */
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) {
        a->ISR = ADC_ISR_EOC;
        a->CR |= ADC_CR_ADSTART;
        while (!(a->ISR & ADC_ISR_EOC)) {}
        sum += a->DR;
    }
    return sum / 16;
}
static uint32_t adc_vrefint(void) { return adc_read(ADC1, 18); }

/* ОУ тракта: OPAMP1 и OPAMP2 — внешняя обвязка (VINP0/VINM0), OPAMP3 — PGA x4 от PB0 */
static void opamp_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    (void)RCC->APB2ENR;
    OPAMP1->CSR = OPAMP_CSR_OPAMPxEN;
    OPAMP2->CSR = OPAMP_CSR_OPAMPxEN;
    OPAMP3->CSR = OPAMP_CSR_OPAMPxEN | (2u << OPAMP_CSR_VMSEL_Pos) | OPAMP_CSR_PGGAIN_0;
}

static void report(uint32_t vref_mv)
{
    static const struct { const char *name; uint8_t adc2; uint8_t ch; const char *exp; } t[] = {
        { "PA0 HV_MONITOR ", 0,  1, "~2440 (HV=0)" },
        { "PA1 V_A (OA1+) ", 0,  2, "1616" },
        { "PA3 OA1-       ", 0,  4, "1616" },
        { "PA2 OA1 out    ", 0,  3, "~2420" },
        { "PA7 V_A (OA2+) ", 1,  4, "1616" },
        { "PA5 OA2-       ", 1, 13, "1616" },
        { "PA6 OA2 out    ", 1,  3, "~1616" },
        { "PB0 V_B (OA3+) ", 0, 15, "99" },
        { "PB1 OA3 out ADC", 0, 12, "~400" },
        { "PB12 TSENS_IN  ", 0, 11, "?" },
    };
    uart_puts("---- analog, mV (expected) ----\n");
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        uint32_t raw = adc_read(t[i].adc2 ? ADC2 : ADC1, t[i].ch);
        uint32_t mv = (raw * vref_mv + 2047) / 4095;
        uart_puts(t[i].name); uart_puts(" raw="); uart_dec(raw);
        uart_puts(" "); uart_dec(mv); uart_puts(" mV  ("); uart_puts(t[i].exp); uart_puts(")\n");
    }
}

static void jump_to_bootloader(void)
{
    GPIOB->BSRR = 1u << (10 + 16); /* ВВ выкл. */
    GPIOA->BSRR = 1u << (8 + 16);
    GPIOA->MODER = (GPIOA->MODER & ~(3u << 16)) | (1u << 16); /* PA8: ШИМ -> выход 0 */
    TIM1->CR1 = 0; TIM1->BDTR = 0;
    RCC->APB2RSTR |= RCC_APB2RSTR_TIM1RST; RCC->APB2RSTR &= ~RCC_APB2RSTR_TIM1RST;
    RCC->APB2ENR &= ~RCC_APB2ENR_TIM1EN;
    uart_puts("-> system bootloader\n");
    uart_flush();

    __disable_irq();
    SysTick->CTRL = 0;
    SysTick->VAL = 0;
    for (int i = 0; i < 8; i++) { NVIC->ICER[i] = 0xFFFFFFFFu; NVIC->ICPR[i] = 0xFFFFFFFFu; }

    ADC1->CR |= ADC_CR_ADDIS;
    ADC2->CR |= ADC_CR_ADDIS;
    OPAMP1->CSR = 0; OPAMP2->CSR = 0; OPAMP3->CSR = 0;
    RCC->AHB2RSTR |= RCC_AHB2RSTR_ADC12RST;  RCC->AHB2RSTR &= ~RCC_AHB2RSTR_ADC12RST;
    RCC->APB2RSTR |= RCC_APB2RSTR_USART1RST; RCC->APB2RSTR &= ~RCC_APB2RSTR_USART1RST;
    RCC->APB2ENR &= ~RCC_APB2ENR_USART1EN;
    RCC->AHB2ENR &= ~RCC_AHB2ENR_ADC12EN;

    /* SYSCLK обратно на HSI, HSE выключить */
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | (1u << RCC_CFGR_SW_Pos);
    while (((RCC->CFGR & RCC_CFGR_SWS) >> RCC_CFGR_SWS_Pos) != 1u) {}
    RCC->CR &= ~(RCC_CR_PLLON | RCC_CR_HSEON);

    /* PA9/PA10 вернуть в аналоговый режим (как после сброса); PB10/PA8 остаются выходами 0 */
    GPIOA->MODER |= (3u << 18) | (3u << 20);

    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    (void)RCC->APB2ENR;
    SYSCFG->MEMRMP = 1u; /* system flash по адресу 0 */

    uint32_t sp = *(volatile uint32_t *)0x1FFF0000u;
    uint32_t pc = *(volatile uint32_t *)0x1FFF0004u;
    __set_MSP(sp);
    __DSB(); __ISB();
    __enable_irq();
    ((void (*)(void))pc)();
    for (;;) {}
}


/* ===================== Управление ВВ =====================
 * HV_CONTROL: PA8 = TIM1_CH1 (AF6), ШИМ 12 бит, 16 МГц / 4096 = 3.9 кГц,
 *             фильтр R302/C312 + R304/C313 (2 x 10к/100н). Амплитуда ШИМ = +3.3V_D,
 *             меряется по каналу VBAT (VBAT на плате = +3.3V_D), ADC1_IN17 = VBAT/3.
 * Уставка:    |V_cw| = 454.5 * V_CONTROL (FB с CW_out; −HV_out под нагрузкой ≈ 0.96 от уставки).
 * HV_ENABLE:  PB10, активный HIGH.
 * Монитор:    PA0 = ADC1_IN1 (с −HV_out), |V| = (zero − V_mon) / 1.615 мВ/В, zero ≈ 2460 мВ.
 */
#define HV_K_SET     454.5f
#define HV_K_MON     1.640f     /* калибровка мультиметром 2026-11-05: −455 В при V_mon 1714 мВ, zero 2460 (расчёт 1.615) */
#define HV_MAX_SET   1200.0f    /* C210–C217 на 500 В: выше 1200 не задирать */
#define HV_TRIP_ABS  1300.0f
#define HV_RAMP      100.0f     /* В/с */
#define HV_MAX_CW    1300.0f    /* потолок уставки CW с подстройкой: 335 В на ступень при C210–C217 на 500 В */
#define HV_TRIM_K    0.001f     /* интегратор подстройки: на шаг 10 мс, τ ≈ 10 с */

/* hv_target/hv_cur — желаемый −HV_out; hv_trim — добавка к уставке CW по монитору
 * (цепочка ОС 1 ГОм нелинейна: при 1000 В на CW выход ≈ 916 В, замер 2026-11-05) */
static float hv_trim, hv_mon_f;
static float hv_target, hv_cur, hv_zero_mv = 2460.0f, vdd_d_mv = 3300.0f;
static int hv_en;
static uint32_t hv_changed_ms;

static void pwm_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;
    (void)RCC->APB2ENR;
    TIM1->PSC = 0;
    TIM1->ARR = 4095;
    TIM1->CCR1 = 0;
    TIM1->CCMR1 = (6u << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE;  /* PWM1 */
    TIM1->CCER = TIM_CCER_CC1E;
    TIM1->BDTR = TIM_BDTR_MOE;
    TIM1->EGR = TIM_EGR_UG;
    TIM1->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~15u) | 6u;                 /* PA8 -> AF6 при CCR1 = 0 */
    GPIOA->MODER = (GPIOA->MODER & ~(3u << 16)) | (2u << 16);
}

static void hv_apply(void)
{
    float cw = hv_cur + hv_trim;
    if (cw > HV_MAX_CW) cw = HV_MAX_CW;
    float d = cw / HV_K_SET * 1000.0f / vdd_d_mv * 4096.0f;
    if (d < 0) d = 0;
    if (d > 4095) d = 4095;
    TIM1->CCR1 = (uint32_t)(d + 0.5f);
}

static void hv_off(const char *why)
{
    GPIOB->BSRR = (1u << (10 + 16)) | (1u << (6 + 16));   /* ВВ и D102 выкл */
    hv_en = 0; hv_target = 0; hv_cur = 0; hv_trim = 0;
    hv_apply();
    uart_puts("HV OFF"); if (why) { uart_puts(": "); uart_puts(why); } uart_puts("\n");
}

static float hv_mon_volts(uint32_t vref_mv, uint32_t *mon_mv)
{
    uint32_t mv = (adc_read(ADC1, 1) * vref_mv + 2047) / 4095;
    if (mon_mv) *mon_mv = mv;
    return (hv_zero_mv - (float)mv) / HV_K_MON;
}

static void put_f(float v)
{
    if (v < 0) { uart_putc('-'); v = -v; }
    uart_dec((uint32_t)(v + 0.5f));
}

/* шаг 10 мс */
static void hv_tick(uint32_t vref_mv)
{
    if (!hv_en) return;
    float step = HV_RAMP * 0.01f;
    if (hv_cur < hv_target) { hv_cur += step; if (hv_cur > hv_target) hv_cur = hv_target; }
    else if (hv_cur > hv_target) { hv_cur -= step; if (hv_cur < hv_target) hv_cur = hv_target; }
    hv_apply();

    float v = hv_mon_volts(vref_mv, 0);
    hv_mon_f += (v - hv_mon_f) * 0.01f;                /* τ ≈ 1 с */
    if (hv_cur == hv_target && hv_target > 0) {       /* подстройка только на стоящей уставке */
        float e = hv_target - hv_mon_f;
        if (e > 3.0f || e < -3.0f)                    /* мёртвая зона: не гонять ШИМ на 1 шаг */
            hv_trim += HV_TRIM_K * e;
        if (hv_trim < -50.0f) hv_trim = -50.0f;
        if (hv_trim > 150.0f) hv_trim = 150.0f;
        if (hv_cur + hv_trim > HV_MAX_CW) hv_trim = HV_MAX_CW - hv_cur;
    }
    if (v > HV_TRIP_ABS) { hv_off("TRIP |V| > 1300 V by monitor"); return; }
    /* выход разряжается медленно (τ ≈ 16 с) — сверять с уставкой, только когда она стоит > 30 с */
    if (hv_cur == hv_target && ms - hv_changed_ms > 30000u && v > hv_target + 150.0f)
        hv_off("TRIP monitor > setpoint + 150 V");
}

static void hv_status(uint32_t vref_mv)
{
    uint32_t mon_mv;
    float v = hv_mon_volts(vref_mv, &mon_mv);
    uart_puts("HV "); uart_puts(hv_en ? "ON " : "off");
    uart_puts(" set=-"); put_f(hv_target);
    uart_puts(" ramp=-"); put_f(hv_cur);
    uart_puts(" trim="); put_f(hv_trim);
    uart_puts(" pwm="); uart_dec(TIM1->CCR1);
    uart_puts(" 3V3D="); put_f(vdd_d_mv);
    uart_puts(" mon="); uart_dec(mon_mv); uart_puts("mV out="); put_f(-v); uart_puts(" V\n");
}

/* Шум базы АЦП: 8192 отсчёта PB1 (ADC1_IN12) с выборкой 12.5 такта (32 МГц / 25 = 1.28 Мвыб/с) */
static uint16_t nbuf[8192];
static void noise(uint32_t vref_mv, ADC_TypeDef *A, uint32_t ch, const char *name)
{
    volatile uint32_t *SM = ch < 10 ? &A->SMPR1 : &A->SMPR2;
    uint32_t sh = (ch % 10) * 3, smpr = *SM;
    *SM = (smpr & ~(7u << sh)) | (2u << sh);                  /* 12.5 такта */
    A->SQR1 = ch << ADC_SQR1_SQ1_Pos;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk; DWT->CYCCNT = 0; DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    __disable_irq();
    uint32_t c0 = DWT->CYCCNT;
    for (int i = 0; i < 8192; i++) {
        A->ISR = ADC_ISR_EOC;
        A->CR |= ADC_CR_ADSTART;
        while (!(A->ISR & ADC_ISR_EOC)) {}
        nbuf[i] = (uint16_t)A->DR;
    }
    uint32_t cyc = DWT->CYCCNT - c0;
    __enable_irq();
    *SM = smpr;
    uart_puts("NOISE fs="); uart_dec((uint32_t)((uint64_t)sysclk * 8192u / cyc)); uart_puts(" Sa/s\n");
    uint32_t sum = 0, mn = 4095, mx = 0;
    for (int i = 0; i < 8192; i++) { sum += nbuf[i]; if (nbuf[i] < mn) mn = nbuf[i]; if (nbuf[i] > mx) mx = nbuf[i]; }
    float mean = sum / 8192.0f, var = 0;
    for (int i = 0; i < 8192; i++) { float d = nbuf[i] - mean; var += d * d; }
    var /= 8192.0f;
    float sd = 0, x = var > 0 ? var : 0;              /* sqrt без libm: Ньютон */
    if (x > 0) { sd = x; for (int k = 0; k < 20; k++) sd = 0.5f * (sd + x / sd); }
    uart_puts("NOISE "); uart_puts(name); uart_puts(" n=8192 mean="); put_f(mean * vref_mv / 4095.0f);
    uart_puts(" mV  sd="); put_f(sd * 1000.0f); uart_puts(" mLSB (");
    put_f(sd * vref_mv * 1000.0f / 4095.0f); uart_puts(" uV)  min="); uart_dec(mn);
    uart_puts(" max="); uart_dec(mx); uart_puts(" LSB\n");
}

static const struct { const char *nm; uint8_t adc2, ch; } npt[] = {
    { "PB1", 0, 12 }, { "PA1", 0, 2 }, { "PA3", 0, 4 }, { "PA2", 0, 3 },
    { "PA7", 1, 4 }, { "PA5", 1, 13 }, { "PA6", 1, 3 }, { "PB0", 0, 15 }, { "PA0", 0, 1 },
};
static int noise_pt(const char *a, uint32_t vref_mv)
{
    if (*a == 0) a = "PB1";
    for (unsigned i = 0; i < sizeof npt / sizeof npt[0]; i++) {
        const char *x = npt[i].nm, *y = a;
        while (*x && *x == *y) { x++; y++; }
        if (*x == 0 && *y == 0) { noise(vref_mv, npt[i].adc2 ? ADC2 : ADC1, npt[i].ch, npt[i].nm); return 1; }
    }
    return 0;
}

static void help(void)
{
    uart_puts("commands: hv <0..1200> | hv off | hvzero | a | n | !boot\n");
}

static void do_cmd(char *l, uint32_t vref_mv)
{
    if (l[0] == 'h' && l[1] == 'v' && l[2] == ' ') {
        char *q = l + 3;
        if (q[0] == 'o') { hv_off("command"); return; }
        if (*q == '-') q++;
        uint32_t v = 0; int ok = 0;
        while (*q >= '0' && *q <= '9') { v = v * 10 + (uint32_t)(*q++ - '0'); ok = 1; }
        if (!ok) { help(); return; }
        if (v > (uint32_t)HV_MAX_SET) { uart_puts("max 1200\n"); return; }
        if (hv_target > 0) hv_trim *= (float)v / hv_target; else hv_trim = 0;
        hv_target = (float)v;
        hv_changed_ms = ms;
        if (!hv_en) { hv_cur = 0; hv_apply(); hv_en = 1; GPIOB->BSRR = (1u << 10) | (1u << 6); }   /* ВВ и D102 вкл */
        uart_puts("HV target -"); uart_dec(v); uart_puts(" V, ramp 100 V/s\n");
    } else if (l[0] == 'h' && l[1] == 'v' && l[2] == 'z') {
        if (hv_en) { uart_puts("turn HV off first\n"); return; }
        uint32_t mon_mv; hv_mon_volts(vref_mv, &mon_mv);
        hv_zero_mv = (float)mon_mv;
        uart_puts("HV monitor zero = "); uart_dec(mon_mv); uart_puts(" mV\n");
    } else if (l[0] == 'd' && (l[1] == 0 || l[1] == ' ')) {   /* d [точка]: замер + выгрузка */
        if (!noise_pt(l[1] ? l + 2 : "", vref_mv)) { help(); return; }
        uart_puts("DUMP 8192\n");
        for (int i = 0; i < 8192; i++) { uart_hex(nbuf[i], 3); if ((i & 31) == 31) uart_puts("\n"); }
        uart_puts("END\n");
    } else if (l[0] == 'n' && (l[1] == 0 || l[1] == ' ')) {   /* n [PB1|PA1|PA3|PA2|PA7|PA5|PA6|PB0|PA0] */
        if (!noise_pt(l[1] ? l + 2 : "", vref_mv)) help();
    } else if (l[0] == 'a' && l[1] == 0) {
        report(vref_mv);
    } else if (l[0]) {
        help();
    }
}

int main(void)
{
    gpio_init();
    SysTick_Config(sysclk / 1000u);
    uart_init();

    uart_puts("\n\n=== gamma MCU board bring-up ===\n");
    uart_puts("DEV_ID 0x"); uart_hex(DBGMCU->IDCODE & 0xFFF, 3);
    uart_puts("  REV 0x");  uart_hex(DBGMCU->IDCODE >> 16, 4);
    uart_puts("\nFLASH "); uart_dec(FLASH_KB); uart_puts(" KB  PKG 0x"); uart_hex(PKG_ID & 0x1F, 2);
    uart_puts("  OPTR 0x"); uart_hex(FLASH->OPTR, 8); uart_puts("\n");

    int hse = hse_start();
    uart_puts(hse ? "HSE ready, switching SYSCLK to HSE...\n" : "HSE FAILED (no HSERDY in 200 ms), staying on HSI\n");
    sysclk_to_pll(hse);
    uart_puts(hse ? "SYSCLK = PLL 128 MHz from HSE\n" : "SYSCLK = PLL 128 MHz from HSI\n");

    opamp_init();
    /* REF35250 (C303 + C322 на NR по 1 мкФ) выходит на режим не сразу; калибровка АЦП
     * на недостоявшейся опоре даёт сдвиг -64 кода на всех каналах (замер 2026-11-05). */
    while (ms < 1500u) {}
    adc_init();
    pwm_init();
    help();

    const char *key = "!boot";
    int kpos = 0;
    char line[32]; int ll = 0;
    uint32_t next = ms, tick = ms, n = 0, vref_mv = 2500;
    for (;;) {
        if (USART1->ISR & USART_ISR_ORE) USART1->ICR = USART_ICR_ORECF;
        if (USART1->ISR & USART_ISR_RXNE_RXFNE) {
            char c = (char)USART1->RDR;
            kpos = (c == key[kpos]) ? kpos + 1 : (c == key[0] ? 1 : 0);
            if (key[kpos] == 0) jump_to_bootloader();
            if (c == '\r' || c == '\n') { line[ll] = 0; if (ll) do_cmd(line, vref_mv); ll = 0; }
            else if (ll < (int)sizeof line - 1) line[ll++] = c;
        }
        if ((int32_t)(ms - tick) >= 0) {
            tick += 10;
            hv_tick(vref_mv);
        }
        if ((int32_t)(ms - next) >= 0) {
            next += 1000;
            /* D102 = индикатор ВВ (горит при включённом), здесь не трогаем. */
            uint32_t raw = adc_vrefint();
            vref_mv = raw ? (3000u * VREFINT_CAL + raw / 2) / raw : 2500;
            {
                float vb = (float)(adc_read(ADC1, 17) * 3u * vref_mv) / 4095.0f;
                static int first = 1;
                if (first) { vdd_d_mv = vb; first = 0; }
                else vdd_d_mv += (vb - vdd_d_mv) / 64.0f;   /* τ ≈ 64 с */
            }
            uart_puts("t="); uart_dec(n++);
            uart_puts(" VREF+="); uart_dec(vref_mv); uart_puts(" ");
            hv_status(vref_mv);
        }
    }
}
