/*
 * gamma_fw: главный цикл.
 *
 * Готово (Task 8): такт 156 МГц (src/hw/clock), выводы ВВ в безопасном состоянии первой строкой
 * (gpio), SysTick 1 мс + DWT (timebase), USART1 600 000 8N1 с приёмом по прерыванию и передачей
 * по DMA (uart), переход в системный загрузчик по "!boot" (boot), аварийный сброс по сбою.
 * Task 9: ОУ тракта и Pt1000 (opamp), служебные АЦП ADC2/4/5 (adc_aux — после 1.5 с),
 * ШИМ HV_CONTROL TIM1 38.1 кГц и HV_ENABLE/D102 (hvpwm); пересчёт и сглаживание замеров,
 * проверка VREF+ и сервисные команды -hv/-hvzero/-hvk/-ptcal/-gst/-diag — core/svc.c.
 * Task 10: сигнальный ADC3_IN1 непрерывно по DMA (adc_sig; запуск сразу после служебных АЦП —
 * тоже не раньше 1.5 с). Главный цикл: adc_sig_take → потерянные порции в мёртвое время
 * (det_add_lost) → det_process_block, такты DWT до/после → загрузка ЦП (статус 0x04, -gst cpu).
 * Импульсы идут в спектр только во время набора (детектор работает всегда: база, σ, осциллограф).
 * Учёт времени набора, живого времени и скоростей — core/acq.c; там же команды -smp, -noise, -osc.
 *
 * Задача 10 мс: adc_aux_poll → svc_aux_10ms (монитор, мВ; NaN — замер устарел) → hv_tick →
 * hvpwm_set, hv_enable_pin(hv_enabled). Задача 1 с: svc_aux_1s (VREF+ 2500 ± 15 мВ — иначе
 * ВВ не включается, при включённом — выкл; при выключенном — перекалибровка АЦП), +3.3V_D τ 60 с,
 * Pt1000 τ 10 с → термокомпенсация (det_set_gain) и T1 в -inf; скорости и загрузка ЦП (acq_tick_1s).
 *
 * Флеш и ЦАП — Task 11–12 (пока заглушки). Буферы, которые проверяет build.sh:
 *  - adc_dma_buf — 2 порции по 8192 отсчёта = 32 КБ, DMA по кругу, в src/hw/adc_sig.c (только ОЗУ:
 *    CCM SRAM на G4 шине DMA недоступна);
 *  - uart_tx_ring — кольцо передачи 8 КБ (≥ PROTO_TX_FRAME_MAX, proto.h) — в src/hw/uart.c;
 *  - стек 4 КБ — _Min_Stack_Size в gamma_fw.ld, куча 2 КБ — _Min_Heap_Size (strtod newlib).
 * Окна осциллографа — в CCM (только ЦП).
 */
#include "stm32g474xx.h"
#include "ccm.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "settings.h"
#include "proto.h"
#include "hv_ctl.h"
#include "temp.h"
#include "detector.h"
#include "spectrum.h"
#include "tpl_3467k.h"
#include "hwcalc.h"
#include "svc.h"
#include "acq.h"
#include "clock.h"
#include "gpio.h"
#include "timebase.h"
#include "uart.h"
#include "boot.h"
#include "opamp.h"
#include "adc_aux.h"
#include "adc_sig.h"
#include "hvpwm.h"

#define OSC_MAX       64u                       /* окон -osc за раз (CCM: 64·32·2 = 4 КБ) */
#define OSC_W         (DET_CAP_PRE_DEF + DET_CAP_POST_DEF)
#define BLK_WAIT_MS   20u                       /* ожидание порции для -noise/-osc raw (порция 2.4…3 мс) */
#define AUX_START_MS  1500u                     /* калибровка АЦП — не раньше 1.5 с (SPEC §3.3) */


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

/* ---- сигнальный АЦП ---- */
static bool      sig_on;
static uint32_t  lost_seen;                     /* потерянные порции, уже отданные в det_add_lost */
static uint32_t  busy_cyc, tick_cyc;            /* такты обработки за секунду; DWT на прошлом тике */

/* ---- служебные замеры ---- */
static svc_aux_t aux;
static bool      aux_on, aux_tried;
static uint16_t  hv_duty;
/* что пришло от служебных АЦП за текущую секунду (опрос — каждые 10 мс) */
static float     sec_vrefint, sec_vbat, sec_pt;
static bool      sec_vrefint_new, sec_vbat_new, sec_pt_new;

/* ---- состояние приложения ---- */
static settings_t live;
static hv_ctl_t   hv;
static CCM_BSS det_t det;                       /* горячие данные детектора — рядом с его кодом */
static CCM_BSS uint16_t  osc_buf[OSC_MAX * OSC_W];
static CCM_BSS det_cap_t osc_info[OSC_MAX];
static bool       osc_on;
static uint16_t   osc_n;
static uint32_t   osc_t0;
static const int16_t (*tpl_now)[DET_NS] = tpl_3467k;
/* учёт набора: время от -sta, накапливается между -sto/-sta, обнуляется -rst; мёртвое время */
static acq_t      acq;

static det_stats_t stats(void)
{
    det_stats_t ds;
    det_get_stats(&det, &ds);
    return ds;
}

/* порция сигнала: потерянные до неё — в мёртвое время, обработка с замером тактов */
static void process_block(const uint16_t *b, size_t n)
{
    uint32_t l = adc_sig_lost();
    if (l != lost_seen) { det_add_lost(&det, (l - lost_seen) * DET_BLK_DEF); lost_seen = l; }
    uint32_t c0 = cycles();
    det_process_block(&det, b, n);
    busy_cyc += cycles() - c0;
}

/* следующая порция (не дольше BLK_WAIT_MS); NULL — АЦП не идёт */
static const uint16_t *wait_block(size_t *n)
{
    uint32_t t0 = millis();
    const uint16_t *b;
    while ((b = adc_sig_take(n)) == NULL)
        if (millis() - t0 > BLK_WAIT_MS) return NULL;
    return b;
}

static void osc_stop(void)
{
    det_set_capture(&det, NULL, NULL, DET_CAP_PRE_DEF, DET_CAP_POST_DEF, 0);
    osc_on = false;
}

/* ---- порт proto ---- */
static void p_tx(const uint8_t *p, size_t n) { uart_tx(p, n); }
static size_t p_tx_free(void) { return uart_tx_free(); }
static uint32_t p_millis(void) { return millis(); }
static bool p_commit(const settings_t *s) { (void)s; return false; }    /* флеш — Task 12 */
static void p_acq_start(void)
{
    det_set_thr(&det, live.thr_codes);         /* авто-порог: σ при старте набора */
    if (osc_on) {                              /* выдача окон блокирует цикл — при наборе нельзя */
        osc_stop();
        proto_send_text("-err osc acq\r\n");
    }
    det_stats_t ds = stats();
    acq_start(&acq, millis(), &ds);
}
static void p_acq_stop(void)
{
    det_stats_t ds = stats();
    acq_stop(&acq, millis(), &ds);
}
static void p_acq_clear(void)
{
    spectrum_clear();
    det_stats_t ds = stats();
    acq_clear(&acq, millis(), &ds);
}
/* "!boot": ВВ выкл (логика и выводы), ответ хосту (gfw.py flash ждёт его, чтобы не слать "!boot"
   повторно уже загрузчику), переход. boot_enter_system сам выключает выводы ВВ и досылает UART. */
static void p_boot(void)
{
    hv_off(&hv, "boot");
    hvpwm_set(0);
    hv_enable_pin(false);
    proto_send_text("-ok boot\r\n");
    boot_enter_system();
}
static void p_status(proto_status_t *st)
{
    det_stats_t ds = stats();
    st->elapsed_s = acq_real_ms(&acq, millis()) / 1000u;
    st->cpu_x10 = acq.cpu_x10;
    st->cps = acq.cps_acc;                     /* принятые за последнюю секунду */
    st->invalid = acq_invalid(&acq, &ds);      /* отбракованные за набор с -rst */
}
static bool p_hv_on(void) { return hv_enabled(&hv); }

static void p_apply(uint32_t what)
{
    /* -defaults (proto пускает только при выключенном ВВ): калибровка монитора — и в hv_ctl */
    if (what == PROTO_APPLY_ALL) hv_set_cal(&hv, live.hv_mon_zero_mv, live.hv_mon_k_mv_per_v);
    if (what & PROTO_APPLY_THR) det_set_thr(&det, live.thr_codes);
    if (what & PROTO_APPLY_TPL) {
        tpl_now = live.tpl_valid ? (const int16_t (*)[DET_NS])live.tpl : tpl_3467k;
        det_set_template(&det, tpl_now);
    }
}

/* ---- сервис (core/svc.c) ---- */
static void s_fill_gst(svc_gst_t *g)
{
    det_stats_t ds = stats();
    uint32_t now = millis();
    g->base = det_baseline(&det);
    g->sigma = det_sigma(&det);
    g->cps_in = acq.cps_in;                    /* за последнюю секунду, в наборе и вне его */
    g->cps_acc = acq.cps_acc;
    g->rej = acq.cps_rej;                      /* отбракованные за секунду: cps_in = cps_acc + rej */
    g->real_s = (float)acq_real_ms(&acq, now) / 1000.0f;
    g->live_s = acq_live_s(&acq, now, &ds);
    g->cpu_pct = (float)acq.cpu_x10 / 10.0f;
    g->lost = adc_sig_lost();
    g->pwm = hv_duty;
    g->flash = proto_flash_status();
}
static bool s_diag(svc_diag_t *d)
{
    float raw[SVC_DIAG_N];
    if (!aux_on || !adc_aux_diag(raw)) return false;
    float vref = svc_vref_mv(&aux);
    for (int i = 0; i < SVC_DIAG_N; i++) d->mv[i] = raw[i] * vref / 4095.0f;
    return true;
}
static const svc_ctx_t svc_ctx = { &hv, &live, &aux, p_millis, s_fill_gst, s_diag };

/* ---- сигнальный тракт (core/acq.c) ---- */
static bool a_acquiring(void) { return acq.run; }
static bool a_ready(void) { return sig_on && adc_sig_running(); }
static uint8_t a_smp_get(void) { return adc_sig_smp() == ADC_SIG_SMP_6C5 ? 6u : 2u; }
static uint32_t a_fs(void) { return adc_sig_fs(); }
static bool a_smp_set(uint8_t smp)
{
    bool ok = adc_sig_set_smp(smp == 6u ? ADC_SIG_SMP_6C5 : ADC_SIG_SMP_2C5);
    acq_set_fs(&acq, adc_sig_fs());
    return ok;
}
static bool a_noise(acq_noise_t *o)
{
    size_t n;
    const uint16_t *b = wait_block(&n);
    if (!b) return false;
    acq_noise(b, n, o);
    process_block(b, n);                       /* порцию не терять */
    return true;
}
/* сырая порция: АЦП на время передачи (≈ 0.3 с при 600 000 бод) стоит, порция не перезаписывается */
static bool a_osc_raw(uint32_t *n_out)
{
    size_t n;
    const uint16_t *b = wait_block(&n);
    if (!b) return false;
    adc_sig_hold();
    proto_send_pulse(b, n);
    bool ok = adc_sig_resume();
    *n_out = (uint32_t)n;
    return ok;
}
static bool a_osc_arm(uint16_t n)
{
    det_set_capture(&det, osc_buf, osc_info, DET_CAP_PRE_DEF, DET_CAP_POST_DEF, n);
    osc_n = n;
    osc_t0 = millis();
    osc_on = true;
    return true;
}
static const acq_ctx_t acq_ctx = {
    a_acquiring, a_ready, a_smp_get, a_smp_set, a_fs, a_noise, a_osc_raw, a_osc_arm, OSC_MAX
};

/* -osc n: записано n окон или вышло время — выдать кадрами 0x02 и строками сведений */
static void osc_poll(void)
{
    if (!osc_on) return;
    uint16_t k = det_capture_count(&det);
    if (k < osc_n && millis() - osc_t0 < ACQ_OSC_TIMEOUT_MS) return;
    char line[128];
    for (uint16_t i = 0; i < k; i++) {
        proto_send_pulse(osc_buf + (size_t)i * OSC_W, OSC_W);
        acq_osc_line(line, sizeof line - 2u, i, &osc_info[i]);
        strcat(line, "\r\n");
        proto_send_text(line);
    }
    osc_stop();
    snprintf(line, sizeof line, "-ok osc done %u\r\n", (unsigned)k);
    proto_send_text(line);
}

static void p_service(const char *cmd, char *reply, size_t cap)
{
    if (svc_handle(cmd, reply, cap)) return;
    if (acq_handle(&acq_ctx, cmd, reply, cap)) return;
    if (strncmp(cmd, "-dac", 4) == 0) snprintf(reply, cap, "-err notyet");    /* Task 11 */
    /* иначе ответ пуст — proto ответит -err unknown */
}

static const proto_port_t port = {
    p_tx, p_millis, p_commit, p_acq_start, p_acq_stop, p_acq_clear, p_boot, p_service, p_status,
    p_apply, p_tx_free, p_hv_on
};

static void emit(uint16_t ch) { if (acq.run) spectrum_add(ch); }   /* в спектр — только в наборе */

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

/* шаг 10 мс: служебные АЦП → монитор → логика ВВ → ШИМ и ENABLE/D102 */
static void task_10ms(uint32_t now)
{
    adc_aux_t ar = { 0 };
    if (aux_on) adc_aux_poll(&ar);
    if (ar.vrefint_new) { sec_vrefint = ar.vrefint_raw; sec_vrefint_new = true; }
    if (ar.vbat_new)    { sec_vbat = ar.vbat_raw;       sec_vbat_new = true; }
    if (ar.pt_new)      { sec_pt = ar.pt_raw;           sec_pt_new = true; }
    float mon = svc_aux_10ms(&aux, ar.mon_raw, ar.mon_new, hv_enabled(&hv), now);
    hv_duty = hv_tick(&hv, mon, aux.vdd_mv, now);
    hvpwm_set(hv_duty);
    hv_enable_pin(hv_enabled(&hv));
}

/* шаг 1 с: VREF+, +3.3V_D, Pt1000, термокомпенсация, скорости и загрузка ЦП, выдача спектра */
static void task_1s(void)
{
    if (aux_on) {
        bool recal = svc_aux_1s(&aux, &hv, sec_vrefint_new ? sec_vrefint : NAN,
                                sec_vbat_new ? sec_vbat : NAN, sec_pt_new ? sec_pt : NAN);
        sec_vrefint_new = sec_vbat_new = sec_pt_new = false;
        hv_set_vref(&hv, svc_vref_mv(&aux));
        if (!hv_enabled(&hv)) { hv_duty = 0; hvpwm_set(0); hv_enable_pin(false); }  /* выкл по VREF+ — сразу */
        if (recal) {                           /* опора не устоялась к калибровке (SPEC §3.3) */
            adc_aux_recalibrate();
            if (sig_on && !acq.run) adc_sig_recalibrate();     /* в наборе — без дыры в потоке */
        }
    }
    pt_state_t pst;
    float raw = aux.pt_raw;                    /* сглаженный τ 10 с; NaN — ещё нет → без поправки */
    det_set_gain(&det, tc_corr_raw(raw, live.pt_rpu_ohm, live.tc_t0_c, live.tc_k1, live.tc_k2, &pst));
    if (pst == PT_OK) proto_set_temperature(pt_temp_c(pt_resistance(raw, live.pt_rpu_ohm)));
    {   /* скорости и загрузка ЦП за секунду */
        det_stats_t ds = stats();
        uint32_t c = cycles();
        acq_tick_1s(&acq, &ds, busy_cyc, c - tick_cyc);
        tick_cyc = c;
        busy_cyc = 0;
    }
    proto_tick_1s();
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
    opamp_init();                              /* тракт и Pt1000 — сразу: устоятся к запуску АЦП */
    hv_enable_pin(false);
    hvpwm_init();                              /* CCR1 = 0: PA8 из «выход 0» в AF без выброса */

    settings_defaults(&live);                  /* флеш — Task 12 */
    if (!settings_valid(&live)) settings_defaults(&live);
    proto_init(&port, &live);
    proto_flash_defaults();
    hv_init(&hv, live.hv_mon_zero_mv, live.hv_mon_k_mv_per_v);
    svc_aux_init(&aux, adc_aux_vrefint_cal());
    svc_init(&svc_ctx);
    det_cfg_t cfg = { live.thr_codes, DET_MINW_DEF, DET_MAXW_DEF, 0.0f, 0 };
    det_init(&det, &cfg, tpl_now, emit);
    spectrum_clear();
    {
        det_stats_t ds = stats();
        acq_init(&acq, DET_FS_HZ, &ds);
    }

    {   /* стартовое сообщение (кадр 0x03): такт и причина сброса; отказ HSE — сообщить (SPEC §2) */
        char m[96];
        snprintf(m, sizeof m, "gamma_fw start: clk %s %lu MHz%s, reset %s\r\n",
                 hse ? "HSE" : "HSI(HSE FAIL)", (unsigned long)(clock_hz() / 1000000u),
                 clock_pll_ok() ? "" : " (PLL FAIL)", reset_cause());
        proto_send_text(m);
    }

    uint32_t t10 = millis(), t1s = t10;
    tick_cyc = cycles();
    for (;;) {
        size_t bn;
        const uint16_t *blk = adc_sig_take(&bn);
        if (blk) {
            process_block(blk, bn);
            osc_poll();
        } else if (osc_on && millis() - osc_t0 >= ACQ_OSC_TIMEOUT_MS) {
            osc_poll();                        /* АЦП встал — выдать, что успели */
        }
        uint8_t rb[64];
        size_t n;
        while ((n = uart_rx(rb, sizeof rb)) != 0)
            proto_rx(rb, n);                   /* "!boot" отсюда не возвращается */
        proto_poll();
        uint32_t now = millis();
        if (!aux_tried && now >= AUX_START_MS) {   /* опора REF35250 устоялась — калибровать АЦП */
            aux_tried = true;
            aux_on = adc_aux_init();
            if (!aux_on) proto_send_text("gamma_fw: aux ADC init FAIL, HV locked\r\n");
            /* сигнальный ADC3 — после служебных: CCR ADC345 пишет adc_aux_init */
            else if (!(sig_on = adc_sig_init(ADC_SIG_SMP_2C5)))
                proto_send_text("gamma_fw: signal ADC init FAIL\r\n");
            else acq_set_fs(&acq, adc_sig_fs());
        }
        if (now - t10 >= 10u) {
            /* отстали (долгая операция) — не догонять пачкой: шаг ВВ рассчитан на 10 мс */
            t10 = (now - t10 >= 50u) ? now : t10 + 10u;
            task_10ms(now);
        }
        if (now - t1s >= 1000u) {
            t1s = (now - t1s >= 2000u) ? now : t1s + 1000u;
            task_1s();
        }
    }
}
