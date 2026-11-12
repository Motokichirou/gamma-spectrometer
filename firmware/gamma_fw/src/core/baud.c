/* baud.c — временная скорость UART (см. baud.h). */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "baud.h"
#include "hwcalc.h"

static const uint32_t ALLOWED[] = { 600000u, 1000000u, 1500000u, 2000000u, 3000000u };

static const baud_port_t *P;
static uint32_t cur = BAUD_DEFAULT;
static uint32_t pend;                 /* 0 — переключать нечего */
static uint32_t last_ms, last_frames;
static bool     seen;                 /* last_ms/last_frames заданы */

void baud_init(const baud_port_t *port)
{
    P = port;
    cur = BAUD_DEFAULT;
    pend = 0;
    seen = false;
}

uint32_t baud_current(void) { return cur; }

bool baud_allowed(uint32_t baud, uint32_t clk_hz)
{
    bool in = false;
    for (unsigned i = 0; i < sizeof ALLOWED / sizeof ALLOWED[0]; i++) if (ALLOWED[i] == baud) in = true;
    if (!in) return false;
    int32_t e = hw_uart_err_ppm(clk_hz, baud);
    return e != INT32_MIN && e <= BAUD_ERR_PPM && e >= -BAUD_ERR_PPM;
}

bool baud_handle(const char *cmd, char *reply, size_t cap)
{
    if (strncmp(cmd, "-baud", 5) != 0 || (cmd[5] && cmd[5] != ' ' && cmd[5] != '\t')) return false;
    const char *a = cmd + 5;
    while (*a == ' ' || *a == '\t') a++;
    if (!*a) { snprintf(reply, cap, "-ok baud %lu", (unsigned long)(pend ? pend : cur)); return true; }
    char *e;
    unsigned long v = (*a >= '0' && *a <= '9') ? strtoul(a, &e, 10) : 0ul;
    if (v) { while (*e == ' ' || *e == '\t') e++; if (*e) v = 0; }
    if (!v || v > 0xFFFFFFFFul || !baud_allowed((uint32_t)v, P && P->clk_hz ? P->clk_hz() : 0u)) {
        snprintf(reply, cap, "-err baud");
        return true;
    }
    pend = (uint32_t)v;               /* ответ уйдёт на старой скорости; переключит baud_poll */
    snprintf(reply, cap, "-ok baud %lu", v);
    return true;
}

static void apply(uint32_t b, uint32_t now_ms)
{
    if (b != cur && P && P->set) P->set(b);
    cur = b;
    last_ms = now_ms;
}

void baud_poll(uint32_t now_ms, uint32_t rx_frames)
{
    if (!seen || rx_frames != last_frames) { seen = true; last_frames = rx_frames; last_ms = now_ms; }
    if (pend) { uint32_t b = pend; pend = 0; apply(b, now_ms); return; }
    if (cur != BAUD_DEFAULT && now_ms - last_ms >= BAUD_IDLE_MS) apply(BAUD_DEFAULT, now_ms);
}
