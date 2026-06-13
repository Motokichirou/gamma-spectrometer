/*
 * stage2_hw.c — этап 2: DAC генерирует импульсы → перемычка PA4(A2)→PA0(A0) →
 * ADC ловит → DSP детектирует пики → реальная гистограмма.
 *
 * Вся периферия инициализируется здесь ручным HAL-кодом (без CubeMX),
 * чтобы не трогать .ioc. Переопределяет слабый хук app_source_poll().
 *
 * Тракт:
 *   TIM6 (1 МГц TRGO) → DAC1_OUT1/PA4 + DMA1_Ch3 (normal, по импульсу)
 *   ADC1_IN1/PA0 (continuous 42.5МГц/15тактов = 2.83 Мвыб/с) + DMA1_Ch2 (circular)
 *   ISR половинок ADC-буфера → dsp_process() → spectrum_add()
 */

#include "main.h"
#include "app.h"
#include "dsp.h"
#include "pulsegen.h"
#include "spectrum.h"
#include "selftest.h"

#define ADC_BUF_LEN     8192u    /* кольцевой буфер ADC, сэмплов */
#define DAC_BASELINE    200u     /* DAC-коды покоя (≈160 мВ) */
#define DSP_THRESHOLD   45u      /* порог: 5 сигма шума железа (sigma~9 ADC-кодов) */
#define DSP_MIN_WIDTH   8u       /* сэмплов (режет шумовые иглы и осколки фронта) */
#define DSP_MAX_WIDTH   250u     /* сэмплов (~88 мкс @ 2.83 Мвыб/с) */
#define DSP_FIT_M       8u       /* окно LSQ-фита = 17 точек (широкий пик этапа 2);
                                    боевая плата @4Мвыб/с: 3 (7 точек) */
#define MEAN_PERIOD_US  200u     /* средний интервал импульсов: 200 мкс ≈ 5000 cps */

ADC_HandleTypeDef hadc1;
DMA_HandleTypeDef hdma_adc1;     /* extern в stm32g4xx_it.c */
DAC_HandleTypeDef hdac1;
DMA_HandleTypeDef hdma_dac1;
TIM_HandleTypeDef htim6;
TIM_HandleTypeDef htim7;         /* планировщик импульсов, 1 мкс тики */

static uint16_t adc_buf[ADC_BUF_LEN];
static uint32_t dac_table[PULSEGEN_TABLE_LEN];   /* uint32: DAC на AHB2 = только 32-битные DMA-записи */
static dsp_t    dsp;
static uint32_t last_rejected;

/* ---- callback детектора: валидный импульс → гистограмма ---- */
static void emit_cb(uint16_t amplitude_half_lsb)
{
    selftest_feed(amplitude_half_lsb);   /* статистика самотеста (если активна) */
    if (app_is_acquiring()) {
        /* dsp выдаёт амплитуду в половинах LSB = готовый канал 0..8190 */
        uint32_t ch = amplitude_half_lsb;
        if (ch >= SPECTRUM_CHANNELS)
            ch = SPECTRUM_CHANNELS - 1u;
        spectrum_add((uint16_t)ch);
        app_count_events(1u);
    }
}

/* ---- инициализация ---- */

static void stage2_gpio_init(void)
{
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    g.Pin  = GPIO_PIN_0 | GPIO_PIN_4;   /* PA0 = ADC1_IN1, PA4 = DAC1_OUT1 */
    g.Mode = GPIO_MODE_ANALOG;
    g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &g);
}

static void stage2_adc_init(void)
{
    RCC_PeriphCLKInitTypeDef pc = {0};
    pc.PeriphClockSelection = RCC_PERIPHCLK_ADC12;
    pc.Adc12ClockSelection  = RCC_ADC12CLKSOURCE_SYSCLK;
    HAL_RCCEx_PeriphCLKConfig(&pc);
    __HAL_RCC_ADC12_CLK_ENABLE();

    hadc1.Instance                   = ADC1;
    hadc1.Init.ClockPrescaler        = ADC_CLOCK_ASYNC_DIV4;   /* 170/4 = 42.5 МГц */
    hadc1.Init.Resolution            = ADC_RESOLUTION_12B;
    hadc1.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
    hadc1.Init.ScanConvMode          = ADC_SCAN_DISABLE;
    hadc1.Init.EOCSelection          = ADC_EOC_SINGLE_CONV;
    hadc1.Init.LowPowerAutoWait      = DISABLE;
    hadc1.Init.ContinuousConvMode    = ENABLE;
    hadc1.Init.NbrOfConversion       = 1;
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConv      = ADC_SOFTWARE_START;
    hadc1.Init.DMAContinuousRequests = ENABLE;
    hadc1.Init.Overrun               = ADC_OVR_DATA_OVERWRITTEN;
    hadc1.Init.OversamplingMode      = DISABLE;
    hadc1.Init.GainCompensation      = 0;
    if (HAL_ADC_Init(&hadc1) != HAL_OK)
        Error_Handler();

    ADC_ChannelConfTypeDef ch = {0};
    ch.Channel      = ADC_CHANNEL_1;                 /* PA0 */
    ch.Rank         = ADC_REGULAR_RANK_1;
    ch.SamplingTime = ADC_SAMPLETIME_2CYCLES_5;      /* (2.5+12.5)/42.5МГц = 2.83 Мвыб/с */
    ch.SingleDiff   = ADC_SINGLE_ENDED;
    ch.OffsetNumber = ADC_OFFSET_NONE;
    if (HAL_ADC_ConfigChannel(&hadc1, &ch) != HAL_OK)
        Error_Handler();

    /* DMA: DMA1_Channel2, circular */
    hdma_adc1.Instance                 = DMA1_Channel2;
    hdma_adc1.Init.Request             = DMA_REQUEST_ADC1;
    hdma_adc1.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_adc1.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_adc1.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_adc1.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_adc1.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
    hdma_adc1.Init.Mode                = DMA_CIRCULAR;
    hdma_adc1.Init.Priority            = DMA_PRIORITY_HIGH;
    if (HAL_DMA_Init(&hdma_adc1) != HAL_OK)
        Error_Handler();
    __HAL_LINKDMA(&hadc1, DMA_Handle, hdma_adc1);

    HAL_NVIC_SetPriority(DMA1_Channel2_IRQn, 3, 0);  /* ниже TIM7: DSP-обработка длинная */
    HAL_NVIC_EnableIRQ(DMA1_Channel2_IRQn);

    HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED);
}

static void stage2_dac_init(void)
{
    __HAL_RCC_DAC1_CLK_ENABLE();

    hdac1.Instance = DAC1;
    if (HAL_DAC_Init(&hdac1) != HAL_OK)
        Error_Handler();

    DAC_ChannelConfTypeDef cfg = {0};
    cfg.DAC_HighFrequency           = DAC_HIGH_FREQUENCY_INTERFACE_MODE_AUTOMATIC;
    cfg.DAC_DMADoubleDataMode       = DISABLE;
    cfg.DAC_SignedFormat            = DISABLE;
    cfg.DAC_SampleAndHold           = DAC_SAMPLEANDHOLD_DISABLE;
    cfg.DAC_Trigger                 = DAC_TRIGGER_T6_TRGO;
    cfg.DAC_OutputBuffer            = DAC_OUTPUTBUFFER_ENABLE;
    cfg.DAC_ConnectOnChipPeripheral = DAC_CHIPCONNECT_EXTERNAL;
    cfg.DAC_UserTrimming            = DAC_TRIMMING_FACTORY;
    if (HAL_DAC_ConfigChannel(&hdac1, &cfg, DAC_CHANNEL_1) != HAL_OK)
        Error_Handler();

    /* DMA: DMA1_Channel3, normal (одна пачка = один импульс) */
    hdma_dac1.Instance                 = DMA1_Channel3;
    hdma_dac1.Init.Request             = DMA_REQUEST_DAC1_CHANNEL1;
    hdma_dac1.Init.Direction           = DMA_MEMORY_TO_PERIPH;
    hdma_dac1.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_dac1.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_dac1.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;   /* AHB2: только 32 бита! */
    hdma_dac1.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
    hdma_dac1.Init.Mode                = DMA_NORMAL;
    hdma_dac1.Init.Priority            = DMA_PRIORITY_MEDIUM;
    if (HAL_DMA_Init(&hdma_dac1) != HAL_OK)
        Error_Handler();
    __HAL_LINKDMA(&hdac1, DMA_Handle1, hdma_dac1);

    /* TIM6: 170 МГц / 170 = 1 МГц, TRGO по update */
    __HAL_RCC_TIM6_CLK_ENABLE();
    htim6.Instance               = TIM6;
    htim6.Init.Prescaler         = 0;
    htim6.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim6.Init.Period            = 169;
    htim6.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_Base_Init(&htim6) != HAL_OK)
        Error_Handler();
    TIM_MasterConfigTypeDef m = {0};
    m.MasterOutputTrigger = TIM_TRGO_UPDATE;
    m.MasterSlaveMode     = TIM_MASTERSLAVEMODE_DISABLE;
    HAL_TIMEx_MasterConfigSynchronization(&htim6, &m);

    /* DAC включается ОДИН РАЗ и больше никогда не выключается:
     * выключение канала (Stop_DMA) отпускает пин в hi-Z, и ADC-выборка
     * высасывает провод к нулю за паузу между импульсами (найдено на железе) */
    HAL_DAC_Start(&hdac1, DAC_CHANNEL_1);
    HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R, DAC_BASELINE);
    HAL_TIM_Base_Start(&htim6);

    /* TIM7 — пуассоновский планировщик: 1 мкс тики, период = интервал
     * до следующего импульса, IRQ по update. SysTick (1 мс) не годится
     * для 5000 cps (средний интервал 200 мкс). */
    __HAL_RCC_TIM7_CLK_ENABLE();
    htim7.Instance               = TIM7;
    htim7.Init.Prescaler         = 169;        /* 170 МГц / 170 = 1 МГц */
    htim7.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim7.Init.Period            = 1000;       /* первый интервал, мкс */
    htim7.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_Base_Init(&htim7) != HAL_OK)
        Error_Handler();
    HAL_NVIC_SetPriority(TIM7_DAC_IRQn, 1, 0);   /* выше ADC: планировщик должен прерывать DSP */
    HAL_NVIC_EnableIRQ(TIM7_DAC_IRQn);
    HAL_TIM_Base_Start_IT(&htim7);
}

/* Выстрел импульса: перезаряжаем DMA-канал вручную, DAC канал не трогаем.
 * DMAEN пере-взводится (после underrun DAC перестаёт слать запросы, RM0440). */
static void dac_fire(void)
{
    __HAL_DMA_DISABLE(&hdma_dac1);
    __HAL_DMA_CLEAR_FLAG(&hdma_dac1, __HAL_DMA_GET_TC_FLAG_INDEX(&hdma_dac1));
    __HAL_DMA_CLEAR_FLAG(&hdma_dac1, __HAL_DMA_GET_HT_FLAG_INDEX(&hdma_dac1));
    __HAL_DMA_CLEAR_FLAG(&hdma_dac1, __HAL_DMA_GET_TE_FLAG_INDEX(&hdma_dac1));
    hdma_dac1.Instance->CNDTR = PULSEGEN_TABLE_LEN;
    hdma_dac1.Instance->CPAR  = (uint32_t)&hdac1.Instance->DHR12R1;
    hdma_dac1.Instance->CMAR  = (uint32_t)dac_table;
    CLEAR_BIT(hdac1.Instance->CR, DAC_CR_DMAEN1);
    __HAL_DAC_CLEAR_FLAG(&hdac1, DAC_FLAG_DMAUDR1);
    __HAL_DMA_ENABLE(&hdma_dac1);
    SET_BIT(hdac1.Instance->CR, DAC_CR_DMAEN1);
}

/* ---- публичная инициализация (зовётся из main USER CODE 2) ---- */
void stage2_init(void)
{
    stage2_gpio_init();
    stage2_adc_init();
    stage2_dac_init();

    pulsegen_init(MEAN_PERIOD_US, DAC_BASELINE);
    dsp_init(&dsp, +1, DSP_THRESHOLD, DSP_MIN_WIDTH, DSP_MAX_WIDTH,
             DSP_FIT_M, emit_cb);

    last_rejected = 0;

    HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc_buf, ADC_BUF_LEN);
}

/* ---- планировщик импульсов: ISR TIM7 с переменным периодом ----
 * Каждое срабатывание = момент очередного пуассоновского события.
 * В main держать нельзя (TX спектра блокирует на ~0.55 с/с),
 * SysTick 1 мс слишком груб для 5000 cps (средний интервал 200 мкс). */
void stage2_tim7_isr(void)
{
    if (__HAL_TIM_GET_FLAG(&htim7, TIM_FLAG_UPDATE) == RESET)
        return;
    __HAL_TIM_CLEAR_FLAG(&htim7, TIM_FLAG_UPDATE);

    bool busy = (hdma_dac1.Instance->CCR & DMA_CCR_EN) &&
                !__HAL_DMA_GET_FLAG(&hdma_dac1,
                                    __HAL_DMA_GET_TC_FLAG_INDEX(&hdma_dac1));
    if (busy) {
        /* предыдущий импульс ещё играет (пуассоновская коллизия) —
         * ретрай через 20 мкс. Реальный детектор получил бы наложение
         * форм; у нас выходит back-to-back, который честно режет PUR. */
        __HAL_TIM_SET_AUTORELOAD(&htim7, 20u - 1u);
        return;
    }

    __HAL_DMA_DISABLE(&hdma_dac1);   /* DAC включён, держит baseline */
    pulsegen_fill(dac_table);
    dac_fire();
    __HAL_TIM_SET_AUTORELOAD(&htim7, pulsegen_next_delay_us() - 1u);
}

/* ---- источник событий: переопределяем слабый хук app.c ---- */
void app_source_poll(uint32_t now)
{
    (void)now;
    /* отбраковка PUR из ISR-домена → в счётчик протокола */
    uint32_t rej = dsp.rejected;
    if (rej != last_rejected) {
        app_count_invalid(rej - last_rejected);
        last_rejected = rej;
    }
}

/* ---- ISR половинок ADC-буфера ---- */
void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc == &hadc1)
        dsp_process(&dsp, &adc_buf[0], ADC_BUF_LEN / 2u);
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc == &hadc1)
        dsp_process(&dsp, &adc_buf[ADC_BUF_LEN / 2u], ADC_BUF_LEN / 2u);
}
