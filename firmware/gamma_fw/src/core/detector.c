/* detector.c — детектор импульсов: быстрый проход + медленный путь (см. detector.h). */
#include "detector.h"
#include "ccm.h"
#include <string.h>
#include <math.h>

#if defined(__ARM_FEATURE_SIMD32) && __ARM_FEATURE_SIMD32
#include <arm_acle.h>
#define UQSUB16(a, b)      ((uint32_t)__uqsub16((a), (b)))
#define SMLALD(a, b, acc)  ((int64_t)__smlald((int32_t)(a), (int32_t)(b), (acc)))
#define SMLAD(a, b, acc)   ((uint32_t)__smlad((int32_t)(a), (int32_t)(b), (int32_t)(acc)))
#define SMUAD(a, b)        ((uint32_t)__smuad((int32_t)(a), (int32_t)(b)))
#else
/* ПК: скалярная модель тех же инструкций — тесты гоняют тот же цикл, что и МК */
static inline uint32_t UQSUB16(uint32_t a, uint32_t b)
{
    uint32_t lo = ((a & 0xFFFFu) > (b & 0xFFFFu)) ? (a & 0xFFFFu) - (b & 0xFFFFu) : 0u;
    uint32_t hi = ((a >> 16) > (b >> 16)) ? (a >> 16) - (b >> 16) : 0u;
    return lo | (hi << 16);
}
static inline int64_t SMLALD(uint32_t a, uint32_t b, int64_t acc)
{
    return acc + (int64_t)(int16_t)(a & 0xFFFFu) * (int16_t)(b & 0xFFFFu)
               + (int64_t)(int16_t)(a >> 16) * (int16_t)(b >> 16);
}
static inline uint32_t SMLAD(uint32_t a, uint32_t b, uint32_t acc)
{
    return acc + (uint32_t)((int32_t)(int16_t)(a & 0xFFFFu) * (int16_t)(b & 0xFFFFu)
                          + (int32_t)(int16_t)(a >> 16) * (int16_t)(b >> 16));
}
static inline uint32_t SMUAD(uint32_t a, uint32_t b) { return SMLAD(a, b, 0u); }
#endif

/* уточнение фазы в fit() без «& 63»: грубые фазы кратны DET_COARSE ≥ 3 и c + 2 < DET_NPH */
typedef char det_coarse_ok[(DET_COARSE >= 3u && DET_NPH % DET_COARSE == 0u) ? 1 : -1];

/* Тело, которое разворачивается в две функции с постоянным параметром (slow/slow_p, fit/fit_p): без
   разбора компилятор выбрасывает его ветки целиком, и прежний путь собирается как раньше. */
#if defined(__GNUC__)
#define DET_INLINE static inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define DET_INLINE static __forceinline
#else
#define DET_INLINE static inline
#endif

/* Развернуть грубый поиск фазы (16 фаз): без развёртки — счётчик, адрес строки и переход на фазу. */
#if defined(__GNUC__) && !defined(__clang__)
#define DET_UNROLL16 _Pragma("GCC unroll 16")
#else
#define DET_UNROLL16
#endif

/* чтение порции словами: псевдоним uint16_t[] без нарушения strict aliasing */
#if defined(__GNUC__)
typedef uint32_t __attribute__((may_alias)) u32_alias;
#else
typedef uint32_t u32_alias;
#endif

/* DET_S_SKIP — бюджет порции исчерпан: остаток порции только считается (см. detector.h);
   DET_S_FRONT, DET_S_TAIL — только при разборе наложений: проверка фронта и хвост по остатку */
enum { DET_S_IDLE = 0, DET_S_PULSE, DET_S_POST, DET_S_GUARD, DET_S_SKIP, DET_S_FRONT, DET_S_TAIL };

static int32_t iround(float x) { return (int32_t)(x >= 0.0f ? x + 0.5f : x - 0.5f); }

/* int64 → float без библиотечного __aeabi_l2f (точно хватает при |x| < 2^47) */
static inline float i64f(int64_t x)
{
    return (float)(int32_t)(x >> 16) * 65536.0f + (float)(uint32_t)(x & 0xFFFF);
}

static inline int16_t clip16(int32_t v)
{
    return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

/* насыщение до int16 в регистре (на ARM — SSAT, один такт) */
#if defined(__ARM_FEATURE_SAT) && __ARM_FEATURE_SAT
#define CLIP16(v) ((int32_t)__ssat((v), 16))
#else
#define CLIP16(v) ((int32_t)clip16(v))
#endif

/* слово из двух int16 по адресу, выровненному на 2 (на Cortex-M4 — LDR без выравнивания) */
static inline uint32_t ld32(const int16_t *p)
{
    uint32_t w;
    memcpy(&w, p, 4);
    return w;
}

void det_init(det_t *d, const det_cfg_t *cfg, const int16_t (*tpl)[DET_NS], det_emit_fn emit)
{
    memset(d, 0, sizeof(*d));
    d->cfg = *cfg;
    d->cfg.thr = DET_THR_DEF;
    det_set_thr(d, cfg->thr);
    det_set_width(d, cfg->min_w, cfg->max_w);
    d->chi2_max = (cfg->chi2_max > 0.0f) ? cfg->chi2_max : 4.0f * (float)DET_NS;
    d->max_fits = cfg->max_fits ? cfg->max_fits : DET_MAX_FITS_DEF;
    d->gain = 1.0f;
    d->emit = emit;
    d->armed = 1;
    d->pile_on = 1;
    det_set_template(d, tpl);
}

void det_set_thr(det_t *d, uint16_t thr)
{
    if (thr == 0u) {                  /* авто: прежний порог работает до конца замера σ */
        d->thr_auto = 1;
        d->auto_left = DET_AUTO_ITERS;
        if (d->cfg.thr == 0u) d->cfg.thr = DET_THR_DEF;
    } else {
        d->thr_auto = 0;
        d->auto_left = 0;
        d->cfg.thr = (thr > DET_THR_AUTO_MAX) ? (uint16_t)DET_THR_AUTO_MAX : thr;
    }
}

uint16_t det_thr(const det_t *d) { return d->cfg.thr; }
int det_thr_measuring(const det_t *d) { return d->auto_left != 0; }

void det_set_width(det_t *d, uint8_t min_w, uint8_t max_w)
{
    if (min_w < 1u)        min_w = 1u;
    if (max_w > DET_MAX_W) max_w = (uint8_t)DET_MAX_W;
    if (max_w < min_w)     max_w = min_w;
    d->cfg.min_w = min_w;
    d->cfg.max_w = max_w;
}

void det_set_capture(det_t *d, uint16_t *buf, det_cap_t *info, uint16_t pre, uint16_t post,
                     uint16_t max_n)
{
    if (pre > DET_CAP_PRE_MAX) pre = DET_CAP_PRE_MAX;
    if (post < 1u) post = 1u;
    d->cap_on = 0;
    d->cap_n = 0;
    d->cap_pre = pre;
    d->cap_post = post;
    d->cap_info = info;
    d->cap_max = buf ? max_n : 0u;
    d->cap_buf = d->cap_max ? buf : NULL;
}

uint16_t det_capture_count(const det_t *d) { return d->cap_n; }

void det_set_template(det_t *d, const int16_t (*tpl)[DET_NS])
{
    for (unsigned p = 0; p < DET_NPH; p++) {
        int32_t sh = 0;
        int64_t hh = 0;                   /* 7·32767² > 2³¹: в int32 переполнялось при |h| > 17 500 */
        for (unsigned k = 0; k < DET_NS; k++) {
            sh += tpl[p][k];
            hh += (int32_t)tpl[p][k] * tpl[p][k];
        }
        det_tph_t *t = &d->tph[p];
        for (unsigned k = 0; k < 3; k++)
            t->h[k] = (uint32_t)(uint16_t)tpl[p][2 * k] | ((uint32_t)(uint16_t)tpl[p][2 * k + 1] << 16);
        t->h[3] = (uint32_t)(uint16_t)tpl[p][6];
        t->sh   = (float)sh;
        t->ihh  = (hh > 0) ? 1.0f / (float)hh : 0.0f;
    }
    d->tpl = tpl;
}

void det_set_gain(det_t *d, float g) { d->gain = g; }

void det_set_base_hold(det_t *d, int on)
{
    d->base_hold = on ? 1u : 0u;
    d->qn = 0; d->qsum = 0; d->qsq = 0;
    d->stk_n = 0; d->stk_s1 = 0; d->stk_s2 = 0;
}
float det_baseline(const det_t *d) { return d->base; }

float det_sigma(const det_t *d)
{
    return (d->var > 0.0f) ? sqrtf(d->var) : 0.0f;
}

void det_get_stats(const det_t *d, det_stats_t *st) { *st = d->st; }

/* ---- разбор хвостовых наложений: список активных хвостов ---- */

/* сумма хвостов на отсчёте q потока, коды (округление): A·s[j−1]/32768, j = q − t0 = 1…DET_TL_LEN */
static inline int32_t tl_pred(const det_t *d, uint32_t q)
{
    int64_t acc = 0x80000000;                        /* коды·2^32 (A·2^17 · Q15), +0.5 — округление */
    for (unsigned k = 0; k < d->tl_n; k++) {
        const uint32_t j = q - d->tl_t[k] - 1u;      /* 0…DET_TL_LEN − 1 — в таблице */
        if (j < DET_TL_LEN)
            acc += (int64_t)d->tl_a[k] * d->tl_r[k][j];   /* SMLAL */
    }
    return (int32_t)(acc >> 32);
}

/* выбросить хвосты, кончившиеся к отсчёту q (j = q − t0 > DET_TL_LEN) */
static void tl_purge(det_t *d, uint32_t q)
{
    unsigned m = 0;
    for (unsigned k = 0; k < d->tl_n; k++) {
        if (q - d->tl_t[k] <= DET_TL_LEN) {
            d->tl_a[m] = d->tl_a[k]; d->tl_t[m] = d->tl_t[k]; d->tl_r[m] = d->tl_r[k];
            m++;
        }
    }
    d->tl_n = (uint8_t)m;
}

/* Подогнанный (до проверки фронта) импульс — в список: максимум buf[imax], A и фаза — pend_*.
   pend_rec — в окне вычитался хвост предыдущего. Полный список — старейший вытесняется. */
static void tl_add(det_t *d)
{
    const uint32_t t0 = d->bpos + d->imax;
    tl_purge(d, t0 - 1u);                            /* остаются дошедшие до окна imax − 1… */
    d->pend_rec = d->tl_n != 0u;
    unsigned m = d->tl_n;
    if (m == DET_TL_MAXACT) {
        for (unsigned k = 1; k < DET_TL_MAXACT; k++) {
            d->tl_a[k - 1] = d->tl_a[k]; d->tl_t[k - 1] = d->tl_t[k]; d->tl_r[k - 1] = d->tl_r[k];
        }
        m--;
    }
    const float a = (d->pend_a < 8191.0f) ? d->pend_a : 8191.0f;    /* A > 0 (fit) */
    d->tl_a[m] = (int32_t)(a * 131072.0f + 0.5f);   /* A·2^17: A·2^17 · s(Q15) = A·s/2^15 · 2^32 */
    d->tl_t[m] = t0;
    d->tl_r[m] = d->tl->s[d->tl->row[d->pend_ph] % DET_TL_ROWS];
    d->tl_n = (uint8_t)(m + 1u);
}

/* проверка фронта пройдена (или прервана сбросом списка): выдача ждущего импульса */
static void front_accept(det_t *d)
{
    d->st.accepted++;
    if (d->pend_rec) d->st.recovered++;
    if (d->emit) d->emit(d->pend_ch);
    if (d->cap_on) {                  /* окно с чужим хвостом эталону не годится */
        if (d->pend_rec) d->cap_on = 0;
        else d->cap_fit = 1;
    }
}

/* Сброс списка хвостов. Ждущий проверки фронта импульс — принят без хвоста; хвост по остатку — в
   обычный GUARD (взвод как был). Без разбора состояний FRONT/TAIL не бывает и список пуст — no-op. */
static void tl_reset(det_t *d)
{
    d->tl_n = 0;
    if (d->state == DET_S_FRONT) {
        front_accept(d);
        d->state = DET_S_GUARD;
        d->guard_left = DET_GUARD;
        d->armed = 0;
    } else if (d->state == DET_S_TAIL) {
        d->state = DET_S_GUARD;
        d->guard_left = DET_GUARD;
    }
}

/* разбор включился: импульс, начатый без него, бросается (номер buf[0] — bpos — не записан) */
static void pa_update(det_t *d)
{
    const uint8_t was = d->pa;
    d->pa = (d->tl != NULL && d->pile_on) ? 1u : 0u;
    if (d->pa && !was && (d->state == DET_S_PULSE || d->state == DET_S_POST)) {
        d->state = DET_S_GUARD;
        d->guard_left = DET_GUARD;
        d->armed = 0;
        d->cap_on = 0;
    }
    tl_reset(d);
}

void det_set_tail(det_t *d, const det_tail_t *t)
{
    d->tl = t;
    pa_update(d);
}

void det_set_pile(det_t *d, int on)
{
    d->pile_on = on ? 1u : 0u;
    pa_update(d);
}

int det_pile_active(const det_t *d) { return d->pa; }

void det_add_lost(det_t *d, uint32_t n)
{
    d->st.dead_samples += n;
    d->pos0 += n;
    d->cap_on = 0;                    /* окно с дырой не годится */
    tl_reset(d);                      /* хвосты до дыры к потоку после неё не относятся */
}

/* авто-порог: round(5σ) по замеру v этого обновления базы (sqrtf → VSQRT при -fno-math-errno) */
static void auto_thr_step(det_t *d, float v)
{
    float t = 5.0f * sqrtf(v > 0.0f ? v : 0.0f) + 0.5f;
    uint16_t thr = (t < (float)DET_THR_AUTO_MIN) ? (uint16_t)DET_THR_AUTO_MIN
                 : (t > (float)DET_THR_AUTO_MAX) ? (uint16_t)DET_THR_AUTO_MAX : (uint16_t)t;
    if (thr == d->cfg.thr || --d->auto_left == 0u) d->auto_left = 0;
    d->cfg.thr = thr;
}

/* Первая порция: база по началу порции (без отсчётов выше среднего + порог). */
static void init_base(det_t *d, const uint16_t *s, size_t n)
{
    size_t m = (n < 256u) ? n : 256u;
    uint32_t sum = 0;
    for (size_t i = 0; i < m; i++) sum += s[i];
    float mean = (float)sum / (float)m;
    float lim = mean + (float)d->cfg.thr;
    float s1 = 0.0f, s2 = 0.0f;
    unsigned c = 0;
    for (size_t i = 0; i < m; i++) {
        if ((float)s[i] <= lim) { s1 += s[i]; c++; }
    }
    if (c) mean = s1 / (float)c;
    c = 0;
    for (size_t i = 0; i < m; i++) {
        float x = (float)s[i] - mean;
        if ((float)s[i] <= lim) { s2 += x * x; c++; }
    }
    d->base = mean;
    d->var  = c > 1u ? s2 / (float)(c - 1u) : 4.0f;
    d->bint = iround(mean);
    d->last = (uint16_t)(d->bint < 0 ? 0 : d->bint);
    for (unsigned k = 0; k < DET_CAP_PRE_MAX; k++) d->tail[k] = d->last;
    d->init = 1;
}

/*
 * Обновление базы и σ² по накопленным спокойным отсчётам (EMA, α = 1/2 на ≥4096 отсчётов).
 * Суммы приводятся к сдвигу bint в целых (без double и библиотечных вызовов): Σ(s−b), Σ(s−b)².
 * Среднее ушло больше чем на порог (ступенька базы вниз) — база ставится сразу, без EMA.
 */
static void update_base(det_t *d)
{
    if (d->qn < DET_BASE_N)
        return;
    if (d->base_hold) {                                /* заморожена: суммы — в корзину */
        d->qn = 0; d->qsum = 0; d->qsq = 0;
        return;
    }
    const int64_t b = d->bint, n = (int64_t)d->qn;
    const int64_t s1 = (int64_t)d->qsum - n * b;
    const int64_t s2 = (int64_t)d->qsq - 2 * b * (int64_t)d->qsum + n * b * b;
    const float fn = (float)d->qn;
    const float m = i64f(s1) / fn;
    float v = i64f(s2) / fn - m * m;
    if (v < 0.0f) v = 0.0f;
    const float mb = (float)d->bint + m;
    const float dm = mb - d->base;
    if (dm > (float)d->cfg.thr || dm < -(float)d->cfg.thr) {
        d->base = mb;
        d->var  = v;
        d->st.base_jumps++;
    } else {
        d->base += 0.5f * dm;
        d->var  += 0.5f * (v - d->var);
    }
    d->bint  = iround(d->base);
    d->qn = 0; d->qsum = 0; d->qsq = 0;
    if (d->auto_left) auto_thr_step(d, v);          /* замер σ — по этому обновлению, без EMA */
}

static inline void stuck_reset(det_t *d) { d->stk_n = 0; d->stk_s1 = 0; d->stk_s2 = 0; }

/*
 * Ступенька базы вверх больше порога: DET_BASE_N отсчётов медленного пути подряд не ниже
 * thr/2 — база по ним, начатый импульс бросается, детектор в IDLE со взводом.
 */
static void rebase_stuck(det_t *d)
{
    const float fn = (float)d->stk_n;
    const float m = (float)d->stk_s1 / fn;
    float v = i64f(d->stk_s2) / fn - m * m;
    d->base = (float)d->bint + m;
    d->var  = (v > 0.0f) ? v : 0.0f;
    d->bint = iround(d->base);
    d->qn = 0; d->qsum = 0; d->qsq = 0;
    d->state = DET_S_IDLE;            /* начатый (и ждущий проверки фронта) импульс бросается */
    d->armed = 1;
    d->guard_left = 0;
    d->st.base_jumps++;
    d->cap_on = 0;
    d->tl_n = 0;                      /* остаток по прежней базе не годится */
    stuck_reset(d);
}

/* ---- запись окон ---- */
static inline uint16_t *cap_rec(det_t *d)
{
    return d->cap_buf + (size_t)d->cap_n * (size_t)(d->cap_pre + d->cap_post);
}

static inline void cap_commit(det_t *d)
{
    if (d->cap_info) d->cap_info[d->cap_n] = d->cap_cur;
    d->cap_n++;
    d->cap_on = 0;
}

/* Новый триггер на отсчёте pos потока: окно в работе либо уже чистое (импульс принят, поток
   прошёл конец окна) — фиксируется, либо испорчено этим триггером — отменяется. */
CCM_FUNC static void cap_trigger(det_t *d, uint32_t pos)
{
    if (!d->cap_on) return;
    if (d->cap_fit && d->cap_got >= d->cap_post
        && (int32_t)(pos - (d->cap_cur.pos + d->cap_post)) >= 0)
        cap_commit(d);
    else
        d->cap_on = 0;
}

/* Триггер s[i]: окно pre до него (из порции или хвоста прошлых) и сколько есть после. */
CCM_FUNC static void cap_start(det_t *d, const uint16_t *s, size_t i, size_t n)
{
    if (d->cap_n >= d->cap_max) { d->cap_on = 0; return; }
    uint16_t *r = cap_rec(d);
    const int pre = d->cap_pre;
    for (int k = 0; k < pre; k++) {
        long idx = (long)i - pre + k;
        r[k] = (idx >= 0) ? s[idx] : d->tail[(long)DET_CAP_PRE_MAX + idx];
    }
    size_t m = n - i;
    if (m > d->cap_post) m = d->cap_post;
    for (size_t k = 0; k < m; k++) r[pre + k] = s[i + k];
    d->cap_got = (uint16_t)m;
    d->cap_on = 1;
    d->cap_fit = 0;
    d->cap_cur.pos = d->pos0 + (uint32_t)i;
}

/* Начало порции: дописать окно, начатое в прошлой. */
CCM_FUNC static void cap_continue(det_t *d, const uint16_t *s, size_t n)
{
    size_t m = (size_t)(d->cap_post - d->cap_got);
    if (m > n) m = n;
    uint16_t *r = cap_rec(d) + d->cap_pre + d->cap_got;
    for (size_t k = 0; k < m; k++) r[k] = s[k];
    d->cap_got = (uint16_t)(d->cap_got + m);
}

/*
 * Скалярное произведение окна (Y0..Y3 — пары int16, см. det_tph_t) на строку эталона: 4 SMLAD.
 * Целое и точное, как прежний поэлементный цикл (|y|·|h|·7 < 2^31 при |y| ≤ 4095+база).
 * Оценка фазы — те же операции float и в том же порядке, что раньше: (float)yh − frac·Σh, затем
 * YH·YH·(1/Σh²); выбор — строго больше, грубые фазы по возрастанию, уточнение −2, −1, +1, +2.
 * Поэтому фаза, A и χ² совпадают с прежней подгонкой бит в бит (tests/test_det_equiv.c).
 */
/*
 * Сравнение без ветвлений по float: sc' = YH·|YH|·(1/Σh²). При YH > 0 это ровно прежнее
 * YH·YH·(1/Σh²) (те же операнды, те же округления), при YH ≤ 0 — ноль или отрицательное (знаковый
 * бит), а прежний код такую фазу пропускал. best ≥ 0, у неотрицательных float порядок битов как
 * у чисел (NaN здесь не бывает) ⇒ «sc' > best» = сравнение битов как int32: одна целая ветка вместо
 * двух VCMP/VMRS. Строка эталона — 4 слова подряд (LDM), Σh и 1/Σh² — следом.
 */
static inline int32_t fbits(float x)
{
    int32_t b;
    memcpy(&b, &x, 4);
    return b;
}
#define DET_YH(t_, yh_) do {                                                 \
        uint64_t w01_, w23_;                       /* строка — двумя LDRD */ \
        memcpy(&w01_, &(t_)->h[0], 8);                                       \
        memcpy(&w23_, &(t_)->h[2], 8);                                       \
        const uint32_t h0_ = (uint32_t)w01_, h1_ = (uint32_t)(w01_ >> 32);   \
        const uint32_t h2_ = (uint32_t)w23_, h3_ = (uint32_t)(w23_ >> 32);   \
        const int32_t i_ =(int32_t)SMLAD(Y0, h0_, SMLAD(Y1, h1_,           \
                                          SMLAD(Y2, h2_, SMUAD(Y3, h3_))));  \
        (yh_) = (float)i_ - frac * (t_)->sh;                                 \
    } while (0)
#define DET_PHASE(pp) do {                                                   \
        const det_tph_t *t_ = &tph[(pp)];                                    \
        float YH_;                                                           \
        DET_YH(t_, YH_);                                                     \
        const int32_t sb_ = fbits(YH_ * fabsf(YH_) * t_->ihh);               \
        if (sb_ > bestb) { bestb = sb_; bp = (pp); }                         \
    } while (0)

/* Подгонка эталона по окну buf[imax−IPK … +NS) и выдача: канал — в emit и в запись окна, иначе
   отбраковка по форме (st.rej_shape, окно записи — в отказ). Прежние fit() и finish() в одной
   функции: результаты не ходят через память. defer ≠ 0 (разбор) — принятый не выдаётся, а ждёт
   проверки фронта: канал, A и фаза — в pend_*. Возврат: 1 — принят, 0 — отбракован. */
DET_INLINE int fit_impl(det_t *d, const int defer)
{
    /* максимум на до-триггерном отсчёте buf[0] (порог снижен или база упала между порциями):
       окно ушло бы за начало буфера — отбраковка по форме */
    if (d->imax < DET_IPK)
        goto reject;
    const int16_t *y = d->buf + (d->imax - DET_IPK);
    const int32_t thr = d->cfg.thr;
    /* выброс 1–2 отсчёта: подряд выше thr/2 вокруг пика меньше DET_MIN_HALF отсчётов — до всего
       остального (раньше Σy, Σy² считались и для выбросов); счёт до DET_MIN_HALF, дальше не нужен */
    unsigned run = 1;
    for (unsigned k = DET_IPK + 1u; k < DET_NS && run < DET_MIN_HALF && 2 * (int32_t)y[k] > thr; k++) run++;
    for (int k = (int)DET_IPK - 1; k >= 0 && run < DET_MIN_HALF && 2 * (int32_t)y[k] > thr; k--) run++;
    if (run < DET_MIN_HALF)
        goto reject;
    const uint32_t Y0 = ld32(y), Y1 = ld32(y + 2), Y2 = ld32(y + 4), Y3 = (uint16_t)y[6];
    const int32_t sy  = (int32_t)SMLAD(Y0, 0x00010001u, SMLAD(Y1, 0x00010001u,
                                       SMLAD(Y2, 0x00010001u, SMLAD(Y3, 0x00010001u, 0u))));
    const int32_t syy = (int32_t)SMLAD(Y0, Y0, SMLAD(Y1, Y1, SMLAD(Y2, Y2, SMLAD(Y3, Y3, 0u))));
    const float frac = d->pbase - (float)d->pbint;      /* y_ист = y − frac */
    const float Syy = (float)syy - 2.0f * frac * (float)sy + (float)DET_NS * frac * frac;
    const det_tph_t *tph = d->tph;

    int32_t bestb = 0;                                              /* биты best (0.0f) */
    int bp = -1;
    DET_UNROLL16
    for (int p = 0; p < (int)DET_NPH; p += (int)DET_COARSE)        /* грубо: 16 фаз */
        DET_PHASE(p);
    if (bp < 0)
        goto reject;
    {                       /* уточнение ±2 из 64; c кратно DET_COARSE ≥ 3 — через 0 переходят только −1, −2 */
        const int c = bp;
        DET_PHASE(c ? c - 2 : (int)DET_NPH - 2);
        DET_PHASE(c ? c - 1 : (int)DET_NPH - 1);
        DET_PHASE(c + 1);
        DET_PHASE(c + 2);
    }

    float best_yh;                                                  /* YH выбранной фазы — заново, */
    DET_YH(&tph[bp], best_yh);                                      /* те же операции, что в цикле */
    const float A = best_yh * tph[bp].ihh * (float)(1u << DET_Q);  /* коды АЦП */
    float best;                                                     /* прежнее best = YH²/Σh² */
    memcpy(&best, &bestb, 4);
    float resid = Syy - best;
    if (resid < 0.0f) resid = 0.0f;
    float var = (d->var > DET_VAR_MIN) ? d->var : DET_VAR_MIN;
    float ea = DET_SHAPE_EPS * A;
    if (A <= 0.0f || resid > d->chi2_max * (var + ea * ea))
        goto reject;
    float c2 = 2.0f * A * d->gain + 0.5f;
    if (c2 < 0.0f) c2 = 0.0f;
    if (c2 > (float)(DET_CHANNELS - 1u)) c2 = (float)(DET_CHANNELS - 1u);
    const uint16_t ch = (uint16_t)c2;
    if (defer)
        goto deferred;
    d->st.accepted++;
    if (d->emit) d->emit(ch);
    if (d->cap_on) {
        d->cap_cur.channel = ch;
        d->cap_cur.amp = A;
        d->cap_cur.phase = (uint8_t)bp;
        d->cap_cur.peak = (int8_t)((int)d->imax - 1);   /* buf[1] — триггер */
        d->cap_cur.base = d->pbase;
        d->cap_fit = 1;           /* фиксация — когда поток пройдёт конец окна без триггера */
    }
    return 1;
deferred:                         /* cap_fit — после проверки фронта (front_accept) */
    d->pend_ch = ch;
    d->pend_a = A;
    d->pend_ph = (uint8_t)bp;
    if (d->cap_on) {
        d->cap_cur.channel = ch;
        d->cap_cur.amp = A;
        d->cap_cur.phase = (uint8_t)bp;
        d->cap_cur.peak = (int8_t)((int)d->imax - 1);
        d->cap_cur.base = d->pbase;
    }
    return 1;
reject:
    d->st.rej_shape++;
    d->cap_on = 0;
    return 0;
}
CCM_FUNC static void fit(det_t *d) { (void)fit_impl(d, 0); }
CCM_FUNC static int fit_p(det_t *d) { return fit_impl(d, 1); }

/*
 * Медленный путь от s[i] до конца порции: импульс, хвост окна, GUARD, затем быстрый проход до
 * следующего триггера — и снова импульс, не выходя отсюда. Выход — конец порции, бюджет исчерпан
 * (SKIP) или пересчёт базы по залипшим. trig ≠ 0 — s[i] — триггер из быстрого прохода.
 *
 * Логика — прежняя (core/dsp.c: гистерезис, ширина, повторный рост на спаде; учёт ступеньки базы —
 * rebase_stuck), результат тот же бит в бит, включая внутреннее состояние между порциями
 * (tests/test_det_equiv.c, tools/cyc --equiv). Ускорение (SPEC §7.2; было ≈ 111 тактов/отсчёт и
 * ≈ 1620 на подгонку по DWT, итого ≈ 4000 на импульс; замеры — tools/cyc):
 *  - состояние машины — в локальных переменных; в det_t — на входе и выходе (раз на порцию, а не на
 *    импульс: быстрый проход между импульсами зовётся отсюда же) и только поля текущего состояния;
 *    отдельные короткие циклы на состояние, чтобы живых переменных хватало регистров;
 *  - ширина не считается: в PULSE каждый отсчёт, кроме последнего, выше порога, т. е.
 *    width = len − 1 (на последнем, ниже порога, — len − 2); проверка max_w — граница цикла plim;
 *  - пороги повторного роста без деления: при lpeak ≥ 0 dev < (3·lpeak)/4 ⇔ 4·dev + 4 ≤ 3·lpeak,
 *    dev > (3·lpeak)/5 ⇔ 3·lpeak < 5·dev (целые, точно); lpeak/4 — один раз на спад;
 *  - счёт ступеньки — без сумм на отсчёт: только счётчик и начало серии залипших (rs); Σdb, Σdb² —
 *    по [rs, p) при выходе, переходе в IDLE или пересчёте (обычно серия к этому моменту уже сброшена
 *    спокойным отсчётом, и суммировать нечего);
 *  - быстрый режим PULSE (pbint == bint, до пересчёта ступеньки далеко): db = dev > thr ≥ thr/2 у
 *    всех отсчётов, кроме последнего, — счёт без сравнений (база заморожена — все отсчёты сбросы);
 *  - триггер в POST/GUARD: импульс начинается до учёта ступеньки на этом отсчёте (тот учитывается уже
 *    в PULSE). Порядок не важен: START ступеньку не трогает, а пересчёт на отсчёте-триггере
 *    невозможен — взвод обнуляет счёт, после него до триггера меньше DET_BUF отсчётов;
 *  - мёртвое время — разностью указателей, в det_t — один раз на выход;
 *  - взведённый спокойный хвост GUARD — по 4 отсчёта на два слова (UQSUB16).
 * det_t — в ОЗУ (main.c): код здесь исполняется из CCM, и данные в CCM стоили бы конфликта шин.
 */

/* Ступенька базы вверх: см. rebase_stuck (db < hl — то же условие, что взвод, или база заморожена).
   На отсчёт — только счёт; суммы Σdb, Σdb² по залипшим — на выходе и при пересчёте, по отсчётам
   [rs, p) этой порции (+ перенесённые из прошлой, пока не было сброса). nxt — следующий отсчёт. */
#define STUCK(v, nxt) do {                                                   \
        if ((int32_t)(v) - bint < hl) {                                      \
            stk_n = 0; rs = (nxt); carried = 0;                              \
        } else if (++stk_n >= DET_BASE_N) {                                  \
            goto rebase;                                                     \
        }                                                                    \
    } while (0)

/* отсчёт dev импульса в буфер, максимум (первый из равных) */
#define PUT(dev) do {                                                        \
        const int32_t c_ = CLIP16(dev);                                      \
        *bp = (int16_t)c_;                                                   \
        if ((dev) > peak) { peak = c_; ip = bp; }                            \
        bp++;                                                                \
    } while (0)

CCM_FUNC static size_t fast_scan(det_t *d, const uint16_t *s, size_t i, size_t n);

/* TAIL разбора (см. slow_impl, метка tail) — отдельной функцией: в теле slow_impl регистров не хватает,
   и цикл по остатку шёл с выгрузкой на стек (≈ 74 такта на отсчёт). Поведение то же, отсчёт в отсчёт:
   pr — tl_pred (при одном хвосте — та же формула A·s[j] + 2^31 >> 32 с j, растущим на отсчёт), взвод и
   триггер с допуском pr/32, счёт ступеньки как STUCK. Возврат — p, причина в c->why: TS_TRIG — p на
   триггере, TS_DONE — хвосты ≤ thr/2 на p, TS_REBASE — p на DET_BASE_N-м залипшем (STUCK до p++),
   TS_END — порция кончилась. (Адрес p в slow_impl не берётся — иначе p жил бы на стеке.) */
enum { TS_END = 0, TS_TRIG, TS_DONE, TS_REBASE };
typedef struct {
    uint32_t armed, stk_n;
    const uint16_t *rs;
    int carried, why;
} det_tsc_t;

#define TS_BODY(PR) do {                                                     \
        const int32_t pr_ = (PR);                                            \
        if (pr_ <= half) { why = TS_DONE; goto ts_out; }                     \
        const int32_t tol_ = pr_ >> 5;                                       \
        const int32_t v_ = *p;                                               \
        const int32_t dev_ = v_ - bint - pr_;                                \
        if (armed && dev_ > thr + tol_) { why = TS_TRIG; goto ts_out; }      \
        if (v_ - bint < hl) {                                                \
            stk_n = 0; rs = p + 1; carried = 0;                              \
        } else if (++stk_n >= DET_BASE_N) {                                  \
            why = TS_REBASE; goto ts_out;                                    \
        }                                                                    \
        if (dev_ < half + tol_) armed = 1;                                   \
    } while (0)

CCM_FUNC static const uint16_t *tail_scan(const det_t *d, const uint16_t *s, const uint16_t *p,
                                          const uint16_t *pe, const int32_t bint, const int32_t thr,
                                          const int32_t half, const int32_t hl, det_tsc_t *c)
{
    uint32_t armed = c->armed, stk_n = c->stk_n;
    const uint16_t *rs = c->rs;
    int carried = c->carried;
    int why = TS_END;
    if (d->tl_n == 1u) {                     /* обычный случай — один хвост */
        const int32_t a = d->tl_a[0];
        const int16_t *const r = d->tl_r[0];
        uint32_t j = d->pos0 + (uint32_t)(p - s) - d->tl_t[0] - 1u;
        for (; p < pe; p++, j++)
            TS_BODY(j < DET_TL_LEN ? (int32_t)((0x80000000LL + (int64_t)a * r[j]) >> 32) : 0);
    } else {
        for (; p < pe; p++)
            TS_BODY(tl_pred(d, d->pos0 + (uint32_t)(p - s)));
    }
ts_out:
    c->armed = armed; c->stk_n = stk_n; c->rs = rs; c->carried = carried; c->why = why;
    return p;
}
#undef TS_BODY

/* суммы счёта ступеньки в det_t: перенесённые (carried) + Σ(s − bint), Σ(s − bint)² по [a, b) */
static inline void stk_flush(det_t *d, uint32_t n_, int carried, const uint16_t *a, const uint16_t *b,
                             int32_t bint)
{
    int32_t s1 = 0;
    int64_t s2 = 0;
    if (n_) {
        if (carried) { s1 = d->stk_s1; s2 = d->stk_s2; }
        for (; a < b; a++) {
            const int32_t db = (int32_t)*a - bint;
            s1 += db;
            s2 += (int64_t)db * db;
        }
    }
    d->stk_n = n_; d->stk_s1 = s1; d->stk_s2 = s2;
}

DET_INLINE size_t slow_impl(det_t *d, const uint16_t *s, size_t i, size_t n, int trig, const uint32_t pa)
{
    const int32_t thr = d->cfg.thr, half = thr / 2, thr2 = 2 * thr;
    const int32_t bint = d->bint;
    const int32_t hl = d->base_hold ? INT32_MAX : half;
    int16_t *const buf = d->buf;
    const uint16_t *p = s + i;                   /* следующий отсчёт */
    const uint16_t *const pe = s + n;
    const uint16_t *p0, *plim;                   /* отрезок PULSE в быстром режиме */
    const int16_t *bmax;                         /* bp > bmax ⇔ width > max_w */
    uint32_t state, armed = 0, guard_left = 0, post_left = 0;
    uint32_t stk_n;                              /* счёт ступеньки */
    const uint16_t *rs = p;                      /* начало залипших в этой порции */
    int carried;                                 /* суммы в det_t — часть текущего счёта */
    uint32_t dead = 0;
    int16_t *bp = buf, *ip = buf;                /* buf + len, buf + imax */
    int32_t peak = 0, lpeak = 0, fall_min = 0, rise, pbint = d->pbint;
    uint32_t falling = 0, pileup = 0, npile = 0;
    int rb = 0;
    int32_t v, dev;

    if (trig) {                              /* триггер быстрого прохода: счёт ступеньки — с нуля */
        stk_n = 0;
        carried = 0;
        state = DET_S_IDLE;
        goto trigger;
    }
    stk_n = d->stk_n;
    carried = 1;
    state = d->state;
    switch (state) {
    case DET_S_PULSE:
        bp = buf + d->len; ip = buf + d->imax;
        peak = d->peak; lpeak = d->lpeak; fall_min = d->fall_min;
        falling = d->falling; pileup = d->pileup; npile = d->npile;
        goto pulse;
    case DET_S_POST:
        bp = buf + d->len; ip = buf + d->imax;
        post_left = d->post_left; armed = d->armed;
        if (pa) goto post_t;
        goto post;
    case DET_S_GUARD:
        armed = d->armed; guard_left = d->guard_left;
        goto guard;
    case DET_S_FRONT:
        armed = d->armed; post_left = d->post_left;
        goto front;
    case DET_S_TAIL:
        armed = d->armed;
        goto tail;
    default:
        return i;
    }

trigger:                                     /* *p — триггер: начать импульс (прежний start_pulse) */
    {
        const size_t it = (size_t)(p - s);
        if (d->cap_on) cap_trigger(d, d->pos0 + (uint32_t)it);
        if (d->fits_left == 0u) {            /* бюджет порции исчерпан */
            state = DET_S_SKIP;
            d->cap_on = 0;
            goto out;
        }
        d->fits_left--;
        int32_t pre = it ? (int32_t)p[-1] : (int32_t)d->last;
        if (d->cap_buf) cap_start(d, s, it, n);
        state = DET_S_PULSE;
        pbint = bint;
        d->pbint = bint;
        d->pbase = d->base;
        if (pa) {
            d->bpos = d->pos0 + (uint32_t)it - 1u;
            pre -= tl_pred(d, d->bpos);
        }
        peak = CLIP16(pre - pbint);
        buf[0] = (int16_t)peak;
        bp = buf + 1; ip = buf;
        lpeak = 0; falling = 0; pileup = 0; npile = 0;
        bmax = buf + d->cfg.max_w + 1;
        if (pa) goto pulse_t;
        goto pulse_f;                        /* pbint == bint, счёт ступеньки — с нуля: быстрый режим */
    }
pulse:
    bmax = buf + d->cfg.max_w + 1;
    if (pa) goto pulse_t;
    /* общий режим и тогда, когда между порциями поменялись порог или ширина: порог вырос при
       falling (быстрый спад не проверяет lpeak ≥ 2·thr) или max_w уменьшилась ниже набранной
       ширины (bp > bmax — plim ниже переполнился бы) */
    if (pbint != bint || (hl == half && stk_n >= DET_BASE_N - DET_BUF) || bp > bmax
        || (falling && lpeak < thr2))
        goto pulse_g;
pulse_f:
    p0 = p;
    /* отсчёт → элемент буфера один к одному, поэтому width > max_w (bp > bmax) — это выход за
       plim: проверка ширины на каждом отсчёте не нужна, только после цикла */
    plim = ((size_t)(pe - p) > (size_t)(bmax - bp)) ? p + (bmax - bp) + 1 : pe;
    if (falling) goto fall_f;
rise_f:                                      /* быстрый режим, до спада */
    if (p < plim) {
        do {
            v = *p++;
            dev = v - pbint;
            const int32_t c = CLIP16(dev);
            *bp = (int16_t)c;
            if (dev > peak) { peak = c; ip = bp; }
            bp++;
            if (dev <= thr) goto term_f;
            if (dev > lpeak) {
                lpeak = c;
            } else if (lpeak >= thr2 && 4 * dev + 4 <= 3 * lpeak) {
                falling = 1;
                fall_min = c;
                goto fall_f;
            }
        } while (p < plim);
    }
    goto end_f;
fall_f:                                      /* быстрый режим, спад: повторный рост = наложение */
    rise = (lpeak / 4 > thr) ? lpeak / 4 : thr;
    if (p < plim) {
        const int32_t l3 = 3 * lpeak;
        do {
            v = *p++;
            dev = v - pbint;
            const int32_t c = CLIP16(dev);
            *bp = (int16_t)c;
            if (dev > peak) { peak = c; ip = bp; }
            bp++;
            if (dev <= thr) goto term_f;
            if (dev < fall_min) {
                fall_min = c;
            } else if (dev > fall_min + rise && l3 < 5 * dev) {
                pileup = 1;
                npile = (uint8_t)(npile + 1u);
                falling = 0;
                lpeak = c;
                goto rise_f;
            }
        } while (p < plim);
    }
end_f:                                       /* кончилась порция или ширина */
    if (hl == half) {
        stk_n += (uint32_t)(p - p0);
    } else {                                 /* база заморожена: каждый отсчёт — сброс счёта */
        stk_n = 0; rs = p; carried = 0;
    }
    dead += (uint32_t)(p - p0);
    if (bp > bmax) goto wide;
    goto out;
term_f:
    if (hl == half)                          /* заморожена — сбросит STUCK ниже */
        stk_n += (uint32_t)(p - p0) - 1u;    /* последний отсчёт — ниже порога: общий учёт */
    dead += (uint32_t)(p - p0);
    STUCK(v, p);
    goto term;

pulse_g:                                     /* общий режим (база сменилась посреди импульса и т. п.) */
    while (p < pe) {
        v = *p;
        STUCK(v, p + 1);
        p++;
        dev = v - pbint;
        dead++;
        PUT(dev);
        if (!falling && dev > lpeak) lpeak = CLIP16(dev);
        if (lpeak >= thr2) {
            if (!falling) {
                if (4 * dev + 4 <= 3 * lpeak) { falling = 1; fall_min = CLIP16(dev); }
            } else if (dev < fall_min) {
                fall_min = CLIP16(dev);
            } else {
                rise = (lpeak / 4 > thr) ? lpeak / 4 : thr;
                if (dev > fall_min + rise && 3 * lpeak < 5 * dev) {
                    pileup = 1;
                    npile = (uint8_t)(npile + 1u);
                    falling = 0;
                    lpeak = CLIP16(dev);
                }
            }
        }
        /* ширина — до конца импульса, как в прежнем коде: width = len − 1 после отсчёта выше
           порога и len − 2 после отсчёта ниже (он в ширину не входит). Штатно на отсчёте ниже
           порога bp ≤ bmax + 1; больше — только если max_w уменьшили посреди импульса */
        if (dev <= thr) {
            if (bp - bmax > 1) goto wide;
            goto term;
        }
        if (bp > bmax) goto wide;
    }
    goto out;

wide:                                        /* шире max_w */
    d->st.rej_width++;
    goto to_guard;
term:                                        /* спад ниже порога — конец импульса */
    if (pileup) {
        d->st.rej_pileup += 1u + npile;
        goto to_guard;
    }
    if ((uint32_t)(bp - buf) - 2u < d->cfg.min_w) {   /* width = len − 2 */
        d->st.rej_width++;
        goto to_guard;
    }
    if (ip + (DET_NS - DET_IPK) > bp) {      /* дописать хвост окна эталона */
        state = DET_S_POST;
        post_left = (uint32_t)(ip + (DET_NS - DET_IPK) - bp);
        armed = (dev < half);
        goto post;
    }
    d->imax = (uint16_t)(ip - buf);
    fit(d);
    goto to_guard;

post:
    while (p < pe) {
        v = *p;
        dev = v - pbint;
        if (armed && dev > thr) goto post_trig;
        STUCK(v, p + 1);
        p++;
        if (dev < half) armed = 1;
        dead++;
        *bp++ = (int16_t)CLIP16(dev);
        if (--post_left == 0u) goto post_end;
    }
    goto out;
post_trig:                                   /* новый импульс до конца окна */
    d->st.rej_pileup++;
    d->cap_on = 0;
    goto trigger;
post_end:
    d->imax = (uint16_t)(ip - buf);
    fit(d);
    state = DET_S_GUARD;                     /* взвод — как был */
    guard_left = DET_GUARD;
    if (!d->cap_fit) d->cap_on = 0;
    goto guard;

/* ---- разбор хвостовых наложений (pa): те же правила по остатку s − bint − хвосты ---- */
pulse_t:                                     /* импульс; конец — ниже порога или окно дописано на спаде */
    while (p < pe) {
        v = *p;
        STUCK(v, p + 1);
        p++;
        dead++;
        dev = v - pbint - tl_pred(d, d->bpos + (uint32_t)(bp - buf));
        PUT(dev);
        if (!falling && dev > lpeak) lpeak = CLIP16(dev);
        if (lpeak >= thr2) {
            if (!falling) {
                if (4 * dev + 4 <= 3 * lpeak) { falling = 1; fall_min = CLIP16(dev); }
            } else if (dev < fall_min) {
                fall_min = CLIP16(dev);
            } else {
                rise = (lpeak / 4 > thr) ? lpeak / 4 : thr;
                if (dev > fall_min + rise && 3 * lpeak < 5 * dev) {
                    pileup = 1;
                    npile = (uint8_t)(npile + 1u);
                    falling = 0;
                    lpeak = CLIP16(dev);
                }
            }
        }
        /* триггер (buf[1]) — начало импульса в любом случае: в GUARD/IDLE он найден по сырым
           отсчётам, а по остатку может оказаться чуть ниже порога */
        if (dev <= thr && bp - buf > 2) {
            if (bp - bmax > 1) goto wide;
            goto term_t;
        }
        if (falling && bp >= ip + (DET_NS - DET_IPK)) goto fall_t;
        if (bp > bmax) goto wide;
    }
    goto out;
term_t:                                      /* ниже порога: width = len − 2 */
    if (pileup) {
        d->st.rej_pileup += 1u + npile;
        goto to_guard;
    }
    if ((uint32_t)(bp - buf) - 2u < d->cfg.min_w) {
        d->st.rej_width++;
        goto to_guard;
    }
    armed = (dev < half);
    if (ip + (DET_NS - DET_IPK) > bp) {      /* дописать хвост окна эталона */
        state = DET_S_POST;
        post_left = (uint32_t)(ip + (DET_NS - DET_IPK) - bp);
        goto post_t;
    }
    goto fit_t;
fall_t:                                      /* окно дописано на спаде, отсчёт выше порога: width = len − 1 */
    if (pileup) {
        d->st.rej_pileup += 1u + npile;
        goto to_guard;
    }
    if ((uint32_t)(bp - buf) - 1u < d->cfg.min_w) {
        d->st.rej_width++;
        goto to_guard;
    }
    armed = 0;
fit_t:
    d->imax = (uint16_t)(ip - buf);
    if (fit_p(d)) goto front_t;
    goto to_guard;
post_t:
    while (p < pe) {
        v = *p;
        dev = v - pbint - tl_pred(d, d->bpos + (uint32_t)(bp - buf));
        if (armed && dev > thr) goto post_trig;
        STUCK(v, p + 1);
        p++;
        if (dev < half) armed = 1;
        dead++;
        *bp++ = (int16_t)CLIP16(dev);
        if (--post_left == 0u) goto post_end_t;
    }
    goto out;
post_end_t:
    d->imax = (uint16_t)(ip - buf);
    if (fit_p(d)) goto front_t;
    state = DET_S_GUARD;                     /* взвод — как был */
    guard_left = DET_GUARD;
    d->cap_on = 0;
    goto guard;
front_t:                                     /* принят: хвост — в список уже сейчас (остаток фронта) */
    tl_add(d);
    state = DET_S_FRONT;
    post_left = DET_FRONT_K;
/* Взвод и триггер в TAIL — с допуском |pred|/32 (η прототипа tools/pileup/pumodel.py): шум может выбрать
   максимумом соседний отсчёт вершины, и строка хвоста съезжает на доли отсчёта — без допуска на крутом
   хвосте остаток не опускается ниже thr/2, взвода нет, и следующий импульс теряется молча. Проверка
   фронта — строго r > thr: с допуском (≈ 12 кодов на +6 у A 600) пропускала бы мелкий второй импульс
   (A 60) с триггером на imax + 7 (Δ 11). */
front:                                       /* DET_FRONT_K отсчётов после окна: триггер — фронт лежал в окне */
    while (p < pe) {
        v = *p;
        const int32_t pr = tl_pred(d, d->pos0 + (uint32_t)(p - s));
        const int32_t tol = (pr < 0 ? -pr : pr) >> 5;
        dev = v - bint - pr;
        if (dev > thr) goto front_rej;
        STUCK(v, p + 1);
        p++;
        if (dev < half + tol) armed = 1;
        dead++;
        if (--post_left == 0u) goto front_ok;
    }
    goto out;
front_rej:                                   /* оба — в брак, как прежняя пара; хвост не записан */
    if (d->tl_n) d->tl_n--;                  /* свой (последний) хвост — из списка */
    d->st.rej_pileup += 2u;
    d->st.rej_front += 2u;
    d->cap_on = 0;
    goto to_guard;
front_ok:
    front_accept(d);
    state = DET_S_TAIL;
tail:                                        /* пока хвосты > thr/2: взвод и триггер по остатку, время живое */
    if (pa) {                                /* цикл — tail_scan (взвод r < thr/2 + pr/32, триггер r > thr + pr/32);
                                                без разбора TAIL не бывает — вызов из slow выброшен */
        det_tsc_t tc = { armed, stk_n, rs, carried, TS_END };
        p = tail_scan(d, s, p, pe, bint, thr, half, hl, &tc);
        armed = tc.armed; stk_n = tc.stk_n; rs = tc.rs; carried = tc.carried;
        if (tc.why == TS_TRIG) goto trigger;
        if (tc.why == TS_DONE) goto tail_end;
        if (tc.why == TS_REBASE) goto rebase;
    }
    goto out;
tail_end:                                    /* дальше — обычный GUARD (хвост не идёт в базу) */
    tl_purge(d, d->pos0 + (uint32_t)(p - s));
    state = DET_S_GUARD;
    guard_left = DET_GUARD;
    goto guard;

to_guard:                                    /* импульс кончился или отбракован */
    state = DET_S_GUARD;
    guard_left = DET_GUARD;
    armed = 0;
    if (!d->cap_fit) d->cap_on = 0;          /* импульс отбракован (ширина, наложение) — окно не нужно */
    /* fallthrough */
guard:
    {
        /* взведено, счёт ступеньки пуст: спокойные отсчёты (s < bint + thr/2, т. е. s ≤ lq) только
           уменьшают guard_left — ни ступеньки, ни мёртвого времени, ни триггера; по 4 за два слова */
        const int32_t lq = bint + half - 1;
        const uint32_t lqu = (uint32_t)(lq > 65535 ? 65535 : (lq < 0 ? 0 : lq));
        const uint32_t ll = lqu | (lqu << 16);
        const int qok = lq >= 0;
        while (p < pe) {
            /* разбор: пока список хвостов не пуст, взвод и триггер — по остатку (ниже), без пропуска по сырым */
            if (armed && guard_left && stk_n == 0u && qok && !(pa && d->tl_n)) {
                const uint16_t *const q = ((size_t)(pe - p) > guard_left) ? p + guard_left : pe;
                const uint16_t *const p1 = p;
                while (q - p >= 4) {
                    uint32_t w0, w1;
                    memcpy(&w0, p, 4);
                    memcpy(&w1, p + 2, 4);
                    if (UQSUB16(w0, ll) | UQSUB16(w1, ll)) break;
                    p += 4;
                }
                if (q - p >= 2) {
                    uint32_t w;
                    memcpy(&w, p, 4);
                    if (!UQSUB16(w, ll)) p += 2;
                }
                while (p < q && *p <= lqu) p++;
                rs = p;                      /* пропущенные — сбросы счёта ступеньки */
                guard_left -= (uint32_t)(p - p1);
                if (guard_left == 0u) goto idle;
                if (p >= pe) break;
            }
            v = *p;
            dev = v - bint;
            if (pa && d->tl_n) {                 /* GUARD после TAIL и т. п.: остаток хвоста ещё не вычтен */
                const uint32_t q = d->pos0 + (uint32_t)(p - s);
                if (q - d->tl_t[d->tl_n - 1u] > DET_TL_LEN) {
                    d->tl_n = 0;                 /* новейший хвост (последний) кончился — кончились все */
                } else {
                    const int32_t pr = tl_pred(d, q);
                    const int32_t tol = (pr < 0 ? -pr : pr) >> 5;   /* допуск — как в TAIL и FRONT */
                    dev -= pr;
                    if (armed && dev > thr + tol) goto trigger;
                    STUCK(v, p + 1);
                    p++;
                    if (dev < half + tol) armed = 1;
                    if (!armed) dead++;
                    if (guard_left) guard_left--;
                    continue;                    /* в IDLE — только когда список пуст */
                }
            }
            if (armed && dev > thr) goto trigger;
            STUCK(v, p + 1);
            p++;
            if (dev < half) armed = 1;
            if (!armed) dead++;
            if (guard_left) guard_left--;
            if (guard_left == 0u && armed) goto idle;
        }
    }
    goto out;

idle:                                        /* GUARD кончился: быстрый проход до следующего триггера —
                                                здесь же, без выхода из медленного пути (состояние
                                                остаётся в регистрах: выход и вход — раз на порцию) */
    state = DET_S_IDLE;
    if (p >= pe) goto out;
    if (stk_n) {                             /* счёт ступеньки — каким был на переходе в IDLE */
        stk_flush(d, stk_n, carried, rs, p, bint);
        carried = 1;
    }
    {
        const size_t j = fast_scan(d, s, (size_t)(p - s), n);
        p = s + j;
        rs = p;
        if (j >= n) goto out;
    }
    stk_n = 0; rs = p; carried = 0;          /* триггер: счёт ступеньки — с нуля */
    goto trigger;

rebase:                                      /* *p — DET_BASE_N-й залипший отсчёт */
    p++;
    rb = 1;
    /* fallthrough */
out:
    d->state = (uint8_t)state;
    switch (state) {
    case DET_S_PULSE:
        d->len = (uint16_t)(bp - buf);
        d->width = (uint16_t)(bp - buf - 1);
        d->imax = (uint16_t)(ip - buf);
        d->peak = (int16_t)peak;
        d->lpeak = (int16_t)lpeak;
        d->fall_min = (int16_t)fall_min;
        d->falling = (uint8_t)falling;
        d->pileup = (uint8_t)pileup;
        d->npile = (uint8_t)npile;
        break;
    case DET_S_POST:
        d->len = (uint16_t)(bp - buf);
        d->imax = (uint16_t)(ip - buf);
        d->post_left = (uint8_t)post_left;
        d->armed = (uint8_t)armed;
        break;
    case DET_S_GUARD:
    case DET_S_IDLE:
        d->armed = (uint8_t)armed;
        d->guard_left = (uint16_t)guard_left;
        break;
    case DET_S_FRONT:
        d->post_left = (uint8_t)post_left;
        d->armed = (uint8_t)armed;
        break;
    case DET_S_TAIL:
        d->armed = (uint8_t)armed;
        break;
    default:
        break;
    }
    stk_flush(d, stk_n, carried, rs, p, bint);   /* залипшие: перенесённые + [rs, p) */
    d->st.dead_samples += dead;
    if (rb) rebase_stuck(d);                 /* база по залипшим, IDLE со взводом */
    return (size_t)(p - s);
}

/* без разбора — прежний медленный путь; с разбором (pa) — отдельная функция */
CCM_FUNC static size_t slow(det_t *d, const uint16_t *s, size_t i, size_t n, int trig)
{
    return slow_impl(d, s, i, n, trig, 0u);
}
CCM_FUNC static size_t slow_p(det_t *d, const uint16_t *s, size_t i, size_t n, int trig)
{
    return slow_impl(d, s, i, n, trig, 1u);
}

/* Быстрый проход: первый отсчёт > bint + thr; суммы Σs, Σs² по пройденным. */
CCM_FUNC static size_t fast_scan(det_t *d, const uint16_t *s, size_t i, size_t n)
{
    int32_t l = d->bint + (int32_t)d->cfg.thr;
    const uint32_t L = (uint32_t)(l < 0 ? 0 : (l > 65535 ? 65535 : l));
    const uint32_t LL = L | (L << 16);
    uint32_t sum = 0;
    int64_t sq = 0;
    size_t j = i;

    if (j < n && ((uintptr_t)(s + j) & 2u) && s[j] <= L) {   /* выравнивание на слово */
        sum += s[j]; sq += (int64_t)s[j] * s[j]; j++;
    }
    /* по 8 отсчётов: одна проверка на четыре слова (ИЛИ результатов UQSUB16); суммы копятся сразу,
       а при выходе (отсчёт выше порога) откатываются — так компилятор держит аккумуляторы в
       регистрах (с суммами «после проверки» он хранил вторую копию Σs² в стеке: 5.3 такта/отсчёт);
       Σs — SMLAD с 0x00010001 (оба полуслова за такт). Отсчёты ≤ 4095 — в int16 без
       знака, Σ по порции ≤ 8192·4095 — в uint32. Было по 2 отсчёта: ≈ 8 тактов/отсчёт (18 % ЦП
       в простое, замер на плате 2026-11-06). */
    if (!((uintptr_t)(s + j) & 2u)) {                          /* не выровнено — s[j] выше порога */
        const u32_alias *p = (const u32_alias *)(const void *)(s + j);
        const u32_alias *pe = p + ((n - j) >> 3) * 4u;
        while (p < pe) {
            const uint32_t w0 = p[0], w1 = p[1], w2 = p[2], w3 = p[3];
            /* суммы — сразу (без второй копии аккумуляторов под ветку), на выходе — откат */
            sum = SMLAD(w0, 0x00010001u, sum);
            sq = SMLALD(w0, w0, sq);
            sum = SMLAD(w1, 0x00010001u, sum);
            sq = SMLALD(w1, w1, sq);
            sum = SMLAD(w2, 0x00010001u, sum);
            sq = SMLALD(w2, w2, sq);
            sum = SMLAD(w3, 0x00010001u, sum);
            sq = SMLALD(w3, w3, sq);
            p += 4;
            if (UQSUB16(w0, LL) | UQSUB16(w1, LL) | UQSUB16(w2, LL) | UQSUB16(w3, LL)) {
                p -= 4;                                        /* откат: точное место — ниже */
                sum -= SMLAD(w0, 0x00010001u, 0u) + SMLAD(w1, 0x00010001u, 0u)
                     + SMLAD(w2, 0x00010001u, 0u) + SMLAD(w3, 0x00010001u, 0u);
                sq -= SMLALD(w0, w0, 0) + SMLALD(w1, w1, 0) + SMLALD(w2, w2, 0) + SMLALD(w3, w3, 0);
                break;
            }
        }
        j = (size_t)((const uint16_t *)(const void *)p - s);
    }
    while (j + 2u <= n) {
        uint32_t w;
        memcpy(&w, s + j, 4);
        if (UQSUB16(w, LL)) break;                             /* один из двух выше порога */
        sum += (w & 0xFFFFu) + (w >> 16);
        sq = SMLALD(w, w, sq);
        j += 2u;
    }
    while (j < n && s[j] <= L) {                               /* хвост и точное место */
        sum += s[j]; sq += (int64_t)s[j] * s[j]; j++;
    }
    d->qsum += sum;
    d->qsq  += (uint64_t)sq;
    d->qn   += (uint32_t)(j - i);
    if (j < n && j > i) {           /* отсчёт перед триггером — уже фронт, из базы убрать */
        uint32_t p = s[j - 1];
        d->qsum -= p; d->qsq -= (uint64_t)p * p; d->qn--;
    }
    return j;
}

/*
 * Бюджет исчерпан на триггере s[i]: остаток порции — мёртвое время, триггеры в нём (подъём выше thr
 * после спада ниже thr/2; первый — сам s[i]) — в rej_budget. Дальше — GUARD, как после импульса.
 */
CCM_FUNC static void skip_rest(det_t *d, const uint16_t *s, size_t i, size_t n)
{
    const int32_t thr = d->cfg.thr, b = d->bint;
    uint32_t cnt = 1, qn = 0, qs = 0, g = 0;
    uint64_t qq = 0;
    int armed = 0, inp = 1;
    for (size_t k = i + 1; k < n; k++) {
        const uint32_t v = s[k];
        int32_t dev = (int32_t)v - b;
        /* спокойные отсчёты — для базы и σ, как в быстром проходе (иначе при перегрузке шумом они
           не обновятся никогда — авто-порог). Как в обычном пути, не берутся: импульс (от триггера
           до спада ниже thr) и DET_GUARD отсчётов после него, пока не взведено (ниже thr/2). */
        if (inp) { if (dev <= thr) { inp = 0; g = DET_GUARD; } }
        else if (g) g--;
        if (!armed) { if (2 * dev < thr) armed = 1; }
        else if (dev > thr) { cnt++; armed = 0; inp = 1; }
        if (!inp && !g && armed) { qn++; qs += v; qq += v * v; }
    }
    d->qn += qn; d->qsum += qs; d->qsq += qq;
    d->st.rej_budget += cnt;
    d->st.dead_samples += (uint64_t)(n - i);
    d->state = DET_S_GUARD;
    d->guard_left = DET_GUARD;
    d->armed = (uint8_t)armed;
    stuck_reset(d);
}

/* хвост потока — для окна pre следующей порции */
static inline void keep_tail(det_t *d, const uint16_t *s, size_t n)
{
    if (n >= DET_CAP_PRE_MAX) {
        for (unsigned k = 0; k < DET_CAP_PRE_MAX; k++) d->tail[k] = s[n - DET_CAP_PRE_MAX + k];
    } else {
        unsigned keep = DET_CAP_PRE_MAX - (unsigned)n;
        for (unsigned k = 0; k < keep; k++) d->tail[k] = d->tail[k + n];
        for (unsigned k = 0; k < n; k++) d->tail[keep + k] = s[k];
    }
}

CCM_FUNC void det_process_block(det_t *d, const uint16_t *s, size_t n)
{
    if (n == 0)
        return;
    if (!d->init)
        init_base(d, s, n);
    d->fits_left = d->max_fits;
    if (d->cap_on && d->cap_got < d->cap_post)
        cap_continue(d, s, n);
    size_t i = 0;
    while (i < n) {
        if (d->state == DET_S_SKIP) {
            skip_rest(d, s, i, n);
            break;
        }
        if (d->state != DET_S_IDLE) {
            i = d->pa ? slow_p(d, s, i, n, 0) : slow(d, s, i, n, 0);
            continue;
        }
        size_t j = fast_scan(d, s, i, n);
        if (j >= n)
            break;
        i = d->pa ? slow_p(d, s, j, n, 1) : slow(d, s, j, n, 1);     /* триггер (счёт ступеньки — с нуля); бюджет исчерпан — DET_S_SKIP */
    }
    d->last = s[n - 1];
    keep_tail(d, s, n);
    d->pos0 += (uint32_t)n;
    if (d->cap_on && d->cap_fit && d->cap_got >= d->cap_post         /* окно прошло без триггера */
        && (int32_t)(d->pos0 - (d->cap_cur.pos + d->cap_post)) >= 0)
        cap_commit(d);
    update_base(d);
}
