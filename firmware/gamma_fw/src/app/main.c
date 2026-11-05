/*
 * gamma_fw: главный цикл.
 *
 * Готово (Task 8): такт 156 МГц (src/hw/clock), выводы ВВ в безопасном состоянии первой строкой
 * (gpio), SysTick 1 мс + DWT (timebase), USART1 600 000 8N1 с приёмом по прерыванию и передачей
 * по DMA (uart), переход в системный загрузчик по "!boot" (boot), аварийный сброс по сбою.
 * Остальное железо — Task 9–12; пока на его месте заглушки, но весь core уже вызывается отсюда:
 * штатная сборка проверяет линковку core, размещение горячего кода в CCM и бюджет ОЗУ (build.sh)
 * вместе с буферами, которые займут драйверы:
 *  - adc_dma_buf — 2 порции по 8192 отсчёта = 32 КБ, DMA по кругу. Только в ОЗУ: CCM SRAM на G4
 *    шине DMA недоступна (Task 10; флаг adc_ready пока всегда 0);
 *  - uart_tx_ring — кольцо передачи 8 КБ (≥ PROTO_TX_FRAME_MAX, proto.h) — в src/hw/uart.c;
 *  - стек 4 КБ — _Min_Stack_Size в gamma_fw.ld, куча 2 КБ — _Min_Heap_Size (strtod newlib).
 * Заглушки служебных АЦП/ШИМ ВВ (mon_mv, hv_pwm_ccr, hv_pin) выводы не трогают: PB10/PA8/PB6
 * остаются выходами 0 из gpio_safe_init до Task 9. Поэтому -hv пока отвечает -err notyet.
 */
#include "stm32g474xx.h"
#include "ccm.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "settings.h"
#include "proto.h"
#include "hv_ctl.h"
#include "temp.h"
#include "detector.h"
#include "spectrum.h"
#include "tpl_3467k.h"
#include "hwcalc.h"
#include "clock.h"
#include "gpio.h"
#include "timebase.h"
#include "uart.h"
#include "boot.h"

#define ADC_DMA_LEN   (2u * DET_BLK_DEF)
#define OSC_MAX       16u


/* Вызывается из Reset_Handler первым делом. Таблица векторов — наша:
   после перехода из системного загрузчика VTOR может смотреть в его ПЗУ. */
void SystemInit(void)
{
    /* Прерывания — выкл до конца инициализации: после Go загрузчика могли остаться его
       включённые прерывания и SysTick, а векторы теперь наши. Включает main после uart_init. */
    __disable_irq();
    gpio_hv_force_off();                        /* ВВ выкл ещё до копирования .data (без глобальных) */
    SCB->VTOR = 0x08000000u;
    /* FPU: сборка hard-float, без доступа к CP10/CP11 первая же VFP-инструкция
       даёт UsageFault. Обязательно. Глобальные переменные здесь не трогать:
       .data/.bss ещё не инициализированы. */
    SCB->CPACR |= (3u << 20) | (3u << 22);
    __DSB();
    __ISB();
}

/* ---- буферы и сигналы драйверов, которых ещё нет (заглушки Task 9–10) ---- */
static uint16_t adc_dma_buf[ADC_DMA_LEN];       /* DMA АЦП — в ОЗУ */
static volatile uint8_t  adc_ready;             /* 1 — первая половина, 2 — вторая (прерывание DMA) */
static volatile float    mon_mv, vdd_mv = 3296.0f, pt_raw;   /* служебные АЦП */
static volatile uint16_t hv_pwm_ccr;            /* TIM1->CCR1 */
static volatile uint8_t  hv_pin;                /* PB10 и D102 */
/* адрес для DMA (CMAR) — заглушка настройки канала; заодно не даёт выбросить буфер при сборке */
static uint16_t *volatile adc_dma_dst;
#define HV_HW_READY 0                           /* Task 9: драйвер ШИМ/ENABLE/монитора готов */

/* ---- состояние приложения ---- */
static settings_t live;
static hv_ctl_t   hv;
static CCM_BSS det_t det;                       /* горячие данные детектора — рядом с его кодом */
static uint16_t   osc_buf[OSC_MAX * (DET_CAP_PRE_DEF + DET_CAP_POST_DEF)];
static det_cap_t  osc_info[OSC_MAX];
static const int16_t (*tpl_now)[DET_NS] = tpl_3467k;
/* время набора: от -sta, накапливается между -sto/-sta, обнуляется -rst */
static bool     acq_run;
static uint32_t acq_since_ms, acq_acc_ms;

/* ---- порт proto ---- */
static void p_tx(const uint8_t *p, size_t n) { uart_tx(p, n); }
static size_t p_tx_free(void) { return uart_tx_free(); }
static uint32_t p_millis(void) { return millis(); }
static bool p_commit(const settings_t *s) { (void)s; return false; }    /* флеш — Task 12 */
static void p_acq_start(void)
{
    det_set_thr(&det, live.thr_codes);         /* авто-порог: σ при старте набора */
    if (!acq_run) { acq_run = true; acq_since_ms = millis(); }
}
static void p_acq_stop(void)
{
    if (acq_run) { acq_acc_ms += millis() - acq_since_ms; acq_run = false; }
}
static void p_acq_clear(void)
{
    spectrum_clear();
    acq_acc_ms = 0;
    acq_since_ms = millis();
}
/* "!boot": ВВ выкл (логика и выводы), ответ хосту (gfw.py flash ждёт его, чтобы не слать "!boot"
   повторно уже загрузчику), переход. boot_enter_system сам выключает выводы ВВ и досылает UART. */
static void p_boot(void)
{
    hv_off(&hv, "boot");
    hv_pin = 0; hv_pwm_ccr = 0;
    proto_send_text("-ok boot\r\n");
    boot_enter_system();
}
static void p_status(proto_status_t *st)
{
    uint32_t ms = acq_acc_ms + (acq_run ? millis() - acq_since_ms : 0u);
    st->elapsed_s = ms / 1000u;
    st->cps = spectrum_total();
}
static bool p_hv_on(void) { return hv_enabled(&hv); }

static void p_apply(uint32_t what)
{
    if (what & PROTO_APPLY_THR) det_set_thr(&det, live.thr_codes);
    if (what & PROTO_APPLY_TPL) {
        tpl_now = live.tpl_valid ? (const int16_t (*)[DET_NS])live.tpl : tpl_3467k;
        det_set_template(&det, tpl_now);
    }
}

static void p_service(const char *cmd, char *reply, size_t cap)
{
    if (strncmp(cmd, "-hv ", 4) == 0) {
        if (!HV_HW_READY) { snprintf(reply, cap, "-err notyet"); return; }
        if (strcmp(cmd + 4, "off") == 0) { hv_off(&hv, "cmd"); snprintf(reply, cap, "-ok hv off"); return; }
        /* строго: целое и ничего после ("-hv abc" через atoi давал 0 — выключение, "12abc" → 12) */
        const char *a = cmd + 4;
        while (*a == ' ' || *a == '\t') a++;
        char *e;
        long v = strtol(a, &e, 10);
        while (*e == ' ' || *e == '\t') e++;
        if (e == a || *e || v < 0 || v > (long)HV_MAX_SET_V || hv_set(&hv, (float)v, millis()) != 0) {
            snprintf(reply, cap, "-err hv");
            return;
        }
        snprintf(reply, cap, "-ok hv %ld", v);
    } else if (strcmp(cmd, "-osc") == 0) {
        det_set_capture(&det, osc_buf, osc_info, DET_CAP_PRE_DEF, DET_CAP_POST_DEF, OSC_MAX);
        snprintf(reply, cap, "-ok osc");
    } else {
        snprintf(reply, cap, "-err notyet");
    }
}

static const proto_port_t port = {
    p_tx, p_millis, p_commit, p_acq_start, p_acq_stop, p_acq_clear, p_boot, p_service, p_status,
    p_apply, p_tx_free, p_hv_on
};

static void emit(uint16_t ch) { spectrum_add(ch); }

/* причина сброса для стартового сообщения (RCC->CSR), флаги затем сбрасываются */
static const char *reset_cause(void)
{
    uint32_t csr = RCC->CSR;
    const char *c = (csr & RCC_CSR_IWDGRSTF) ? "iwdg" : (csr & RCC_CSR_WWDGRSTF) ? "wwdg"
                  : (csr & RCC_CSR_SFTRSTF)  ? "sw"   : (csr & RCC_CSR_BORRSTF)  ? "bor"
                  : (csr & RCC_CSR_OBLRSTF)  ? "obl"  : (csr & RCC_CSR_LPWRRSTF) ? "lpwr"
                  : (csr & RCC_CSR_PINRSTF)  ? "pin"  : "go";   /* без флагов — переход из загрузчика по Go */
    RCC->CSR |= RCC_CSR_RMVF;
    return c;
}

int main(void)
{
    gpio_safe_init();                          /* ПЕРВЫМ: PB10 = PA8 = PB6 = 0 — ВВ и D102 выкл (SPEC §3) */
    /* после Go загрузчика могли остаться его прерывания — наши векторы их не ждут
       (PRIMASK уже взведён в SystemInit) */
    __disable_irq();
    SysTick->CTRL = 0;
    for (unsigned i = 0; i < sizeof NVIC->ICER / sizeof NVIC->ICER[0]; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFu;
        NVIC->ICPR[i] = 0xFFFFFFFFu;
    }
    bool hse = clock_init();                   /* 156 МГц; false — от HSI */
    timebase_init(clock_hz());
    uart_init(HW_UART_BAUD);
    __enable_irq();

    adc_dma_dst = adc_dma_buf;                 /* «DMA АЦП по кругу» — Task 10 */
    settings_defaults(&live);                  /* флеш — Task 12 */
    if (!settings_valid(&live)) settings_defaults(&live);
    proto_init(&port, &live);
    proto_flash_defaults();
    hv_init(&hv, live.hv_mon_zero_mv, live.hv_mon_k_mv_per_v);
    det_cfg_t cfg = { live.thr_codes, DET_MINW_DEF, DET_MAXW_DEF, 0.0f, 0 };
    det_init(&det, &cfg, tpl_now, emit);
    spectrum_clear();

    {   /* стартовое сообщение (кадр 0x03): такт и причина сброса; отказ HSE — сообщить (SPEC §2) */
        char m[96];
        snprintf(m, sizeof m, "gamma_fw start: clk %s %lu MHz%s, reset %s\r\n",
                 hse ? "HSE" : "HSI(HSE FAIL)", (unsigned long)(clock_hz() / 1000000u),
                 clock_pll_ok() ? "" : " (PLL FAIL)", reset_cause());
        proto_send_text(m);
    }

    uint32_t t10 = millis(), t1s = t10;
    for (;;) {
        uint8_t r = adc_ready;
        if (r) {
            adc_ready = 0;
            det_process_block(&det, adc_dma_buf + (r - 1u) * DET_BLK_DEF, DET_BLK_DEF);
        }
        uint8_t rb[64];
        size_t n;
        while ((n = uart_rx(rb, sizeof rb)) != 0)
            proto_rx(rb, n);                   /* "!boot" отсюда не возвращается */
        proto_poll();
        uint32_t now = millis();
        if (now - t10 >= 10u) {
            /* отстали (долгая операция) — не догонять пачкой: шаг ВВ рассчитан на 10 мс */
            t10 = (now - t10 >= 50u) ? now : t10 + 10u;
            hv_pwm_ccr = hv_tick(&hv, mon_mv, vdd_mv, now);
            hv_pin = hv_enabled(&hv);
        }
        if (now - t1s >= 1000u) {
            t1s = (now - t1s >= 2000u) ? now : t1s + 1000u;
            pt_state_t pst;
            float raw = pt_raw;
            det_set_gain(&det, tc_corr_raw(raw, live.pt_rpu_ohm, live.tc_t0_c, live.tc_k1, live.tc_k2, &pst));
            if (pst == PT_OK) proto_set_temperature(pt_temp_c(pt_resistance(raw, live.pt_rpu_ohm)));
            proto_tick_1s();
        }
    }
}
