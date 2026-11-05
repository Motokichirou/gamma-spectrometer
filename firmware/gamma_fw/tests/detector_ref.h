/*
 * detector_ref.h — ЭТАЛОН ДЛЯ ТЕСТОВ: копия src/core/detector.h до ускорения медленного пути
 * (main @ c247193), имена det_/DET_ → detref_/DETREF_. Не править (см. detector_ref.c).
 *
 * detector.h — детектор импульсов сигнального АЦП (SPEC §7.1–7.4). Переносимый модуль.
 *
 * Обработка порциями в два уровня:
 *  1) быстрый проход: поиск первого отсчёта выше база + порог (по два отсчёта за слово,
 *     на ARM — UQSUB16) и суммы Σs, Σs² по спокойным участкам для базы и σ шума;
 *  2) медленный путь — только от найденного отсчёта: машина состояний из core/dsp.c
 *     (гистерезис, мин./макс. ширина, повторный рост на спаде = наложение), затем
 *     подгонка эталона 64 фазы × 7 отсчётов (грубо по 16 фазам, уточнение ±2) и
 *     отбраковка по χ² (выбросы помехи 257 кГц шириной 1–2 отсчёта).
 *
 * Подгонка: y = s − база (база — среднее по спокойным участкам), МНК с ОДНИМ параметром
 * y ≈ A·h. Свободный член B (как в dsp.c) здесь не нужен: база меряется по тысячам
 * отсчётов, а не EMA, которую тянет хвост; с B разброс амплитуды на 30 % больше
 * (σ 4.0 против 2.9 канала у Cs-137 при σ шума 1.8 LSB).
 *
 * χ² = невязка / (σ² + (ε·A)²), ε = DETREF_SHAPE_EPS: член ε·A учитывает неточность эталона
 * (шаг фазы 1/64 отсчёта даёт до 1.2 % от A) — без него крупные импульсы
 * отбраковывались бы целиком (χ² до 500 у 3.5 МэВ).
 *
 * Отбраковка выбросов по форме — до χ²: вокруг пика подряд должно быть не меньше DETREF_MIN_HALF
 * отсчётов выше thr/2. У настоящего импульса третий отсчёт (спад) ≈ 0.33…0.45·A, у выброса
 * шириной 1–2 отсчёта там шум. Одного χ² при σ шума 1.8 LSB не хватает: выброс 2×12 LSB
 * подгоняется с χ² ≈ 14, а 99-й перцентиль χ² настоящих импульсов ≈ 15 — проходило ≈80 %
 * выбросов шириной 2, с отсечкой ≈0.7 %. Цена — порог по энергии: при thr = 9 импульсы
 * A 15 (≈17 кэВ) принимаются на 59 % вместо 74 %, A 20 (≈23 кэВ) на 95 % вместо 98.7 %,
 * от 32 кэВ — без потерь (test_threshold_losses).
 *
 * Скачок базы (SPEC §7.3, ревью Task 6):
 *  - вверх больше порога: каждый отсчёт выше thr, спокойных нет, база по ним не обновится.
 *    Если подряд DETREF_BASE_N отсчётов медленного пути не опустились ниже thr/2, база
 *    пересчитывается по ним же, детектор — в IDLE со взводом (st.base_jumps++);
 *  - вниз: спокойные отсчёты есть; если их среднее ушло от базы больше чем на thr,
 *    база ставится сразу (без EMA), тоже st.base_jumps++.
 *  Итог: после ступеньки любого знака база верна не позже чем через 2 порции.
 *  ⚠ В SPEC (§7.3, §7.4, §10) эти решения и однопараметрическая подгонка с ε·A ещё не
 *  записаны — по плану это Task 11 (SPEC правится там же, где журнал).
 *
 * Эталон меняется на лету detref_set_template() (сервис -tplok): база и статистика не трогаются.
 * Так же на лету — порог detref_set_thr() и ширина detref_set_width() (сервис -thr, -sta).
 *
 * Авто-порог (cfg.thr = 0 или detref_set_thr(d, 0); в настройках thr_codes = 0): σ шума меряется
 * по следующим DETREF_BASE_N спокойным отсчётам (≈ 1.2 мс), порог = round(5σ) в пределах
 * DETREF_THR_AUTO_MIN…DETREF_THR_AUTO_MAX. Спокойные отсчёты отбираются по текущему порогу (сверху
 * они срезаны), поэтому при σ много больше прежнего порога первый замер занижен — замер
 * повторяется, пока порог не перестанет меняться (не больше DETREF_AUTO_ITERS раз). До конца замера
 * работает прежний порог (с самого начала — DETREF_THR_DEF). SPEC §7.3: «σ мерить при старте
 * набора» — приложение зовёт detref_set_thr(d, settings.thr_codes) по -sta.
 *
 * Бюджет времени (SPEC §7.2, ревью): на порцию — не больше cfg.max_fits запусков медленного пути
 * (триггеров; по умолчанию DETREF_MAX_FITS_DEF: порция 8192 отсчётов при 3.4667 Мвыб/с = 2.36 мс =
 * 368 640 тактов при 156 МГц, 70 % из них по ≈1300 тактов на импульс ⇒ 198, ≈ 84 кимп/с).
 * Бюджет считается на вызов detref_process_block — звать порциями по DETREF_BLK_DEF. Исчерпан —
 * остаток порции не обрабатывается: каждый триггер в нём (подъём выше thr после спада ниже thr/2)
 * — в st.rej_budget, все отсчёты до конца порции — в st.dead_samples; со следующей порции —
 * обычный режим (в GUARD, как после импульса). Спокойные отсчёты остатка (вне импульсов и их
 * хвостов DETREF_GUARD) идут в базу и σ, как в быстром проходе: иначе при перегрузке шумом (σ много
 * больше порога) база и замер авто-порога не обновились бы никогда.
 *
 * Запись окон (осциллограф, набор эталона; SPEC §7.6): detref_set_capture() — для принятых
 * импульсов пишется окно pre отсчётов до триггера и post начиная с триггера (сырые коды АЦП;
 * триггер — первый отсчёт выше база + thr, в записи — индекс pre) и сведения detref_cap_t
 * (номер отсчёта триггера, канал, амплитуда, фаза эталона, положение максимума, база).
 * Записывается окно без другого триггера внутри: новый триггер, пока окно не дописано или
 * импульс не подогнан, отменяет прежнюю запись (наложение испортило бы эталон).
 * Окно, начатое в одной порции, дописывается в следующей.
 *
 * Импульс на стыке порций переносится: состояние живёт в detref_t между вызовами.
 * Канал = round(2·A·gain), 0…8191 (половины LSB).
 */
#ifndef DETECTOR_REF_H
#define DETECTOR_REF_H

#include <stdint.h>
#include <stddef.h>

#define DETREF_NS        7u     /* отсчётов в окне эталона */
#define DETREF_IPK       1u     /* позиция максимума в окне */
#define DETREF_NPH       64u    /* фаз эталона */
#define DETREF_Q         14u    /* формат эталона Q14 */
#define DETREF_COARSE    4u     /* шаг грубого поиска фазы (64/4 = 16 фаз) */
#define DETREF_GUARD     16u    /* отсчётов после импульса не идут в базу (отрицательный хвост) */
#define DETREF_MAX_W     64u    /* потолок max_w */
#define DETREF_BUF       (DETREF_IPK + DETREF_MAX_W + DETREF_NS + 2u)
#define DETREF_SHAPE_EPS 0.01f  /* точность эталона относительно A (для χ²) */
#define DETREF_VAR_MIN   0.25f  /* нижний предел σ² (квантование, тихий вход) */
#define DETREF_CHANNELS  8192u
#define DETREF_BASE_N    4096u  /* спокойных отсчётов на обновление базы; столько же «залипших» — пересчёт */
#ifndef DETREF_MIN_HALF
#define DETREF_MIN_HALF  3u     /* отсчётов окна выше thr/2 (отсев выбросов 1–2 отсчёта) */
#endif

/* умолчания (SPEC §7.3) */
#define DETREF_THR_DEF   9u     /* ≈ 5σ шума базы */
#define DETREF_MINW_DEF  2u
#define DETREF_MAXW_DEF  16u

/* авто-порог 5σ */
#define DETREF_THR_AUTO_MIN 3u
#define DETREF_THR_AUTO_MAX 1000u
#define DETREF_AUTO_ITERS   4u

/* бюджет времени на порцию (SPEC §7.2) */
#define DETREF_BLK_DEF    8192u        /* порция DMA */
#define DETREF_FS_HZ      3466667u     /* выборка ADC3 */
#define DETREF_CPU_HZ     156000000u
#define DETREF_FIT_CYC    1300u        /* тактов медленного пути на импульс (цель SPEC §7.2) */
#define DETREF_BUDGET_PCT 70u          /* доля длительности порции */
#define DETREF_MAX_FITS_DEF ((uint16_t)((uint64_t)DETREF_BLK_DEF * DETREF_CPU_HZ / DETREF_FS_HZ                                      * DETREF_BUDGET_PCT / 100u / DETREF_FIT_CYC))   /* = 198 */

/* запись окон */
#define DETREF_CAP_PRE_MAX 16u         /* потолок pre (хвост прошлых порций) */
#define DETREF_CAP_PRE_DEF 8u          /* SPEC §7.6: −8…+24 */
#define DETREF_CAP_POST_DEF 24u

typedef void (*detref_emit_fn)(uint16_t channel);          /* 0..8191, половины LSB */

typedef struct {
    uint16_t thr;            /* порог над базой, коды АЦП; 0 — авто 5σ */
    uint8_t  min_w, max_w;   /* ширина — число отсчётов выше порога */
    float    chi2_max;       /* <= 0 — по умолчанию 4·DETREF_NS */
    uint16_t max_fits;       /* бюджет: триггеров медленного пути на порцию; 0 — DETREF_MAX_FITS_DEF */
} detref_cfg_t;

typedef struct {
    uint32_t accepted, rej_pileup, rej_shape, rej_width;
    uint32_t rej_budget;     /* триггеров, пропущенных из-за исчерпания бюджета порции */
    uint32_t base_jumps;     /* скачков базы (пересчёт без EMA) */
    uint64_t dead_samples;
} detref_stats_t;

/* сведения о записанном окне */
typedef struct {
    uint32_t pos;            /* номер отсчёта триггера в потоке (с detref_init, по модулю 2^32;
                                потерянные порции detref_add_lost учитываются) */
    uint16_t channel;        /* канал, как ушёл в detref_emit_fn */
    uint8_t  phase;          /* фаза эталона 0…DETREF_NPH−1 (строка tpl), выбранная подгонкой */
    int8_t   peak;           /* отсчёт максимума относительно триггера; окно эталона
                                tpl[phase][0…NS) легло на отсчёты peak − DETREF_IPK … */
    float    amp;            /* A, коды АЦП (до усиления gain) */
    float    base;           /* база, от которой считан импульс, коды АЦП */
} detref_cap_t;

typedef struct {
    /* конфигурация */
    detref_cfg_t cfg;
    float     chi2_max;
    float     gain;
    const int16_t (*tpl)[DETREF_NS];
    detref_emit_fn emit;
    float     tpl_ihh[DETREF_NPH];   /* 1/Σh² по строке */
    int32_t   tpl_sh[DETREF_NPH];    /* Σh по строке */

    /* база и шум */
    int       init;
    float     base, var;
    int32_t   bint;               /* round(base) на текущую порцию */
    uint16_t  last;               /* последний отсчёт предыдущей порции (до-триггерный) */
    uint32_t  qn;                 /* спокойные отсчёты с прошлого обновления базы */
    uint64_t  qsum, qsq;          /* Σs, Σs² по ним */
    uint32_t  stk_n;              /* отсчётов медленного пути подряд не ниже thr/2 */
    int32_t   stk_s1;             /* Σ(s − bint) по ним */
    int64_t   stk_s2;             /* Σ(s − bint)² */
    int32_t   pbint;              /* база, от которой считан текущий импульс */
    float     pbase;

    /* авто-порог, бюджет */
    uint8_t   base_hold;          /* 1 — база и σ заморожены (detref_set_base_hold) */
    uint8_t   auto_left;          /* > 0 — идёт замер σ для авто-порога */
    uint8_t   thr_auto;           /* порог задан как «авто» */
    uint16_t  max_fits, fits_left;

    /* запись окон */
    uint16_t *cap_buf;
    detref_cap_t *cap_info;
    uint16_t  cap_pre, cap_post, cap_max, cap_n;
    uint8_t   cap_on;             /* окно в работе (дописывается и/или ждёт подгонки) */
    uint8_t   cap_fit;            /* 0 — подгонки ещё не было, 1 — импульс принят */
    uint16_t  cap_got;            /* отсчётов после триггера уже записано */
    detref_cap_t cap_cur;
    uint32_t  pos0;               /* номер отсчёта s[0] текущей порции */
    uint16_t  tail[DETREF_CAP_PRE_MAX]; /* последние отсчёты прошлых порций */

    /* медленный путь */
    uint8_t   state;              /* DETREF_S_* в detector.c */
    uint8_t   armed;
    uint8_t   falling, pileup, npile;
    uint8_t   post_left;
    uint16_t  width;              /* отсчётов выше порога */
    uint16_t  guard_left;
    int16_t   peak, lpeak, fall_min;
    uint16_t  imax, len;
    int16_t   buf[DETREF_BUF];       /* s − bint */

    detref_stats_t st;
} detref_t;

void  detref_init(detref_t *d, const detref_cfg_t *cfg, const int16_t (*tpl)[DETREF_NS], detref_emit_fn emit);
/* замена эталона (или его содержимого) на лету: пересчёт Σh и 1/Σh²; база, σ и счёт не трогаются.
 * Вызывать между порциями (не из прерывания DMA посреди detref_process_block). */
void  detref_set_template(detref_t *d, const int16_t (*tpl)[DETREF_NS]);
/* порог на лету (база, σ и статистика не трогаются): thr > 0 — фиксированный (до DETREF_THR_AUTO_MAX),
 * 0 — авто 5σ: новый замер σ по ближайшим спокойным отсчётам (см. выше). Между порциями. */
void  detref_set_thr(detref_t *d, uint16_t thr);
uint16_t detref_thr(const detref_t *d);             /* действующий порог, коды */
int   detref_thr_measuring(const detref_t *d);      /* 1 — идёт замер σ для авто-порога */
/* ширина на лету (те же пределы, что в detref_init: min ≥ 1, max ≤ DETREF_MAX_W, max ≥ min).
 * Вступает в силу со следующего импульса. Между порциями. */
void  detref_set_width(detref_t *d, uint8_t min_w, uint8_t max_w);
/* Запись окон принятых импульсов: buf — max_n·(pre + post) отсчётов, info — max_n записей
 * (может быть NULL). pre ≤ DETREF_CAP_PRE_MAX, post ≥ 1. Счёт записанных — с нуля; по заполнении
 * запись останавливается. buf == NULL или max_n == 0 — выключить. Между порциями. */
void  detref_set_capture(detref_t *d, uint16_t *buf, detref_cap_t *info, uint16_t pre, uint16_t post,
                      uint16_t max_n);
uint16_t detref_capture_count(const detref_t *d);   /* записано окон */
/* порция; импульс на стыке переносится в следующую */
void  detref_process_block(detref_t *d, const uint16_t *s, size_t n);
/* потерянная порция (переполнение DMA): n отсчётов — в мёртвое время */
void  detref_add_lost(detref_t *d, uint32_t n);
float detref_baseline(const detref_t *d);
float detref_sigma(const detref_t *d);
void  detref_get_stats(const detref_t *d, detref_stats_t *st);
/* термокомпенсация: канал = round(2·A·g) */
void  detref_set_gain(detref_t *d, float g);
/* Заморозить (on ≠ 0) или отпустить базу и σ: пока заморожены, спокойные отсчёты не обновляют
 * базу (суммы сбрасываются), пересчёт по «залипшим» отсчётам (ступенька вверх) не делается,
 * замер авто-порога стоит. Для самотеста ЦАП (main.c): спуск ЦАП после каждого импульса —
 * отрицательные импульсы до −400 кодов, по ним среднее «спокойных» ушло бы на десятки кодов,
 * σ — в сотни. Емкость C318 не пропускает постоянную составляющую, поэтому база до серии верна
 * и во время неё. Между порциями. */
void  detref_set_base_hold(detref_t *d, int on);

#endif
