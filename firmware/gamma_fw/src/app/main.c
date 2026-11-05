/*
 * gamma_fw: главный цикл. Драйверы железа (src/hw) — Task 8–12; пока на их месте заглушки,
 * но весь core уже вызывается отсюда: штатная сборка проверяет линковку core, размещение
 * горячего кода в CCM и бюджет ОЗУ (build.sh) вместе с буферами, которые займут драйверы.
 *
 * Заглушки железа ничего не трогают: флаги «порция АЦП готова», «пришли байты», «тик 10 мс»
 * будут выставлять прерывания DMA/UART/SysTick; пока они всегда 0 и цикл крутится вхолостую.
 * Буферы драйверов зарезервированы здесь же и учитываются в бюджете ОЗУ:
 *  - adc_dma_buf — 2 порции по 8192 отсчёта = 32 КБ, DMA по кругу. Только в ОЗУ: CCM SRAM на G4
 *    шине DMA недоступна;
 *  - uart_tx_ring — кольцо передачи 8 КБ (≥ PROTO_TX_FRAME_MAX, proto.h);
 *  - стек 4 КБ — _Min_Stack_Size в gamma_fw.ld, куча 2 КБ — _Min_Heap_Size (strtod newlib).
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

#define ADC_DMA_LEN   (2u * DET_BLK_DEF)
#define UART_RING_LEN 8192u
#define OSC_MAX       16u

typedef char chk_ring[(UART_RING_LEN >= PROTO_TX_FRAME_MAX) ? 1 : -1];

/* Вызывается из Reset_Handler первым делом. Таблица векторов — наша:
   после перехода из системного загрузчика VTOR может смотреть в его ПЗУ. */
void SystemInit(void)
{
    SCB->VTOR = 0x08000000u;
    /* FPU: сборка hard-float, без доступа к CP10/CP11 первая же VFP-инструкция
       даёт UsageFault. Обязательно. Глобальные переменные здесь не трогать:
       .data/.bss ещё не инициализированы. */
    SCB->CPACR |= (3u << 20) | (3u << 22);
    __DSB();
    __ISB();
}

/* ---- буферы драйверов (заглушки) ---- */
static uint16_t adc_dma_buf[ADC_DMA_LEN];       /* DMA АЦП — в ОЗУ */
static uint8_t  uart_tx_ring[UART_RING_LEN];
static volatile uint32_t uart_tx_head, uart_tx_tail;
static volatile uint8_t  adc_ready;             /* 1 — первая половина, 2 — вторая (прерывание DMA) */
static volatile uint8_t  tick_10ms, tick_1s;    /* SysTick */
static volatile uint32_t ms_now;
static volatile float    mon_mv, vdd_mv = 3296.0f, pt_raw;   /* служебные АЦП */
static volatile uint16_t hv_pwm_ccr;            /* TIM1->CCR1 */
static volatile uint8_t  hv_pin;                /* PB10 и D102 */
static uint8_t  rx_tmp[64];
static volatile uint32_t rx_len;
/* адреса для DMA (CMAR) — заглушка настройки каналов; заодно не даёт выбросить буферы при сборке */
static const uint8_t *volatile uart_dma_src;
static uint16_t *volatile adc_dma_dst;

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
static void p_tx(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {               /* заглушка кольца: без DMA, при переполнении — теряем */
        uint32_t h = uart_tx_head, nh = (h + 1u) % UART_RING_LEN;
        if (nh == uart_tx_tail) return;
        uart_tx_ring[h] = p[i];
        uart_tx_head = nh;
    }
    uart_dma_src = uart_tx_ring + uart_tx_tail;    /* «запуск DMA» с хвоста кольца */
}
static size_t p_tx_free(void)
{
    return (UART_RING_LEN - 1u) - ((uart_tx_head - uart_tx_tail) % UART_RING_LEN);
}
static uint32_t p_millis(void) { return ms_now; }
static bool p_commit(const settings_t *s) { (void)s; return false; }    /* флеш — Task 12 */
static void p_acq_start(void)
{
    det_set_thr(&det, live.thr_codes);         /* авто-порог: σ при старте набора */
    if (!acq_run) { acq_run = true; acq_since_ms = ms_now; }
}
static void p_acq_stop(void)
{
    if (acq_run) { acq_acc_ms += ms_now - acq_since_ms; acq_run = false; }
}
static void p_acq_clear(void)
{
    spectrum_clear();
    acq_acc_ms = 0;
    acq_since_ms = ms_now;
}
static void p_boot(void) { hv_off(&hv, "boot"); hv_pin = 0; hv_pwm_ccr = 0; }   /* переход — Task 8 */
static void p_status(proto_status_t *st)
{
    uint32_t ms = acq_acc_ms + (acq_run ? ms_now - acq_since_ms : 0u);
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
        if (strcmp(cmd + 4, "off") == 0) { hv_off(&hv, "cmd"); snprintf(reply, cap, "-ok hv off"); return; }
        /* строго: целое и ничего после ("-hv abc" через atoi давал 0 — выключение, "12abc" → 12) */
        const char *a = cmd + 4;
        while (*a == ' ' || *a == '\t') a++;
        char *e;
        long v = strtol(a, &e, 10);
        while (*e == ' ' || *e == '\t') e++;
        if (e == a || *e || v < 0 || v > (long)HV_MAX_SET_V || hv_set(&hv, (float)v, ms_now) != 0) {
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

int main(void)
{
    adc_dma_dst = adc_dma_buf;                 /* «DMA АЦП по кругу» — Task 10 */
    settings_defaults(&live);                  /* флеш — Task 12 */
    if (!settings_valid(&live)) settings_defaults(&live);
    proto_init(&port, &live);
    proto_flash_defaults();
    hv_init(&hv, live.hv_mon_zero_mv, live.hv_mon_k_mv_per_v);
    det_cfg_t cfg = { live.thr_codes, DET_MINW_DEF, DET_MAXW_DEF, 0.0f, 0 };
    det_init(&det, &cfg, tpl_now, emit);
    spectrum_clear();

    for (;;) {
        uint8_t r = adc_ready;
        if (r) {
            adc_ready = 0;
            det_process_block(&det, adc_dma_buf + (r - 1u) * DET_BLK_DEF, DET_BLK_DEF);
        }
        uint32_t n = rx_len;
        if (n) { rx_len = 0; proto_rx(rx_tmp, n > sizeof rx_tmp ? sizeof rx_tmp : n); }
        proto_poll();
        if (tick_10ms) {
            tick_10ms = 0;
            hv_pwm_ccr = hv_tick(&hv, mon_mv, vdd_mv, ms_now);
            hv_pin = hv_enabled(&hv);
        }
        if (tick_1s) {
            tick_1s = 0;
            pt_state_t pst;
            float raw = pt_raw;
            det_set_gain(&det, tc_corr_raw(raw, live.pt_rpu_ohm, live.tc_t0_c, live.tc_k1, live.tc_k2, &pst));
            if (pst == PT_OK) proto_set_temperature(pt_temp_c(pt_resistance(raw, live.pt_rpu_ohm)));
            proto_tick_1s();
        }
    }
}
