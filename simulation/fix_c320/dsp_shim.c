/* dsp_shim.c — обёртка НАСТОЯЩЕГО firmware/core/dsp.c для Монте-Карло на ПК (Python ctypes).
 * Сборка: build_dsp.cmd (MSVC x64). Логика детектора не дублируется — компилируется исходник прошивки. */
#include "dsp.h"

static dsp_t g;
static uint16_t *g_out;
static size_t g_cap, g_n;

static void emit(uint16_t a)
{
    if (g_n < g_cap)
        g_out[g_n] = a;
    g_n++;
}

__declspec(dllexport) void shim_init(int thr, int minw, int maxw, int fitm)
{
    dsp_init(&g, +1, (uint16_t)thr, (uint16_t)minw, (uint16_t)maxw, (uint8_t)fitm, emit);
}

__declspec(dllexport) size_t shim_process(const uint16_t *s, size_t n, uint16_t *out, size_t cap)
{
    g_out = out;
    g_cap = cap;
    g_n = 0;
    dsp_process(&g, s, n);
    return g_n;
}

__declspec(dllexport) void shim_stats(unsigned *acc, unsigned *rej, unsigned *bl)
{
    *acc = g.accepted;
    *rej = g.rejected;
    *bl = dsp_baseline(&g);
}
