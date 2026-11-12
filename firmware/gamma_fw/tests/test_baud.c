/* test_baud.c — временная скорость UART (-baud, core/baud.c): разбор, ответ, переключение после ответа, сторож. */
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "t.h"
#include "baud.h"

static uint32_t clk = 156000000u;
static uint32_t set_calls, set_last;
static void p_set(uint32_t b) { set_calls++; set_last = b; }
static uint32_t p_clk(void) { return clk; }
static const baud_port_t port = { p_set, p_clk };

static char rep[64];
static const char *h(const char *c)
{
    rep[0] = 0;
    if (!baud_handle(c, rep, sizeof rep)) strcpy(rep, "<not baud>");
    return rep;
}
static void reset(void) { clk = 156000000u; set_calls = 0; set_last = 0; baud_init(&port); }

static void test_parse(void)
{
    reset();
    CHECK(strcmp(h("-baud"), "-ok baud 600000") == 0);
    CHECK(strcmp(h("-baudx 3000000"), "<not baud>") == 0);
    CHECK(strcmp(h("-hv 100"), "<not baud>") == 0);
    const char *bad[] = { "-baud 115200", "-baud 0", "-baud abc", "-baud 3000000x", "-baud -600000",
                          "-baud 3000000 1", "-baud 2500000", "-baud 99999999999" };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) CHECK(strcmp(h(bad[i]), "-err baud") == 0);
    CHECK(set_calls == 0);
    const uint32_t ok[] = { 600000u, 1000000u, 1500000u, 2000000u, 3000000u };
    for (unsigned i = 0; i < 5; i++) {
        char c[32], r[32];
        snprintf(c, sizeof c, "-baud %lu", (unsigned long)ok[i]);
        snprintf(r, sizeof r, "-ok baud %lu", (unsigned long)ok[i]);
        CHECK(strcmp(h(c), r) == 0);
        CHECK(baud_allowed(ok[i], 156000000u));
    }
    CHECK(strcmp(h("-baud   3000000  "), "-ok baud 3000000") == 0);
    /* делитель: 156 МГц / 3 М = 52 ровно; при 16 МГц (HSI без PLL) 3 М не годится (BRR 5 < 16) */
    CHECK(!baud_allowed(3000000u, 16000000u));
    CHECK(!baud_allowed(600000u, 16000000u));       /* BRR 27: ошибка −1.2 % */
}

static void test_switch_after_reply(void)
{
    reset();
    baud_poll(0, 0);
    CHECK(strcmp(h("-baud 3000000"), "-ok baud 3000000") == 0);
    CHECK(set_calls == 0);                 /* ответ ещё не ушёл — не переключать в разборе */
    CHECK(baud_current() == 600000u);
    CHECK(strcmp(h("-baud"), "-ok baud 3000000") == 0);   /* запрошенная */
    baud_poll(10, 1);
    CHECK(set_calls == 1 && set_last == 3000000u);
    CHECK(baud_current() == 3000000u);
    baud_poll(20, 1);
    CHECK(set_calls == 1);
    /* та же скорость — хук не зовётся */
    h("-baud 3000000");
    baud_poll(30, 2);
    CHECK(set_calls == 1);
    /* обратно */
    h("-baud 600000");
    baud_poll(40, 3);
    CHECK(set_calls == 2 && set_last == 600000u && baud_current() == 600000u);
}

static void test_watchdog(void)
{
    reset();
    baud_poll(1000, 5);
    h("-baud 2000000");
    baud_poll(1000, 6);
    CHECK(baud_current() == 2000000u);
    /* кадры идут — держим */
    for (uint32_t t = 1000, f = 6; t < 20000; t += 1000) { baud_poll(t, ++f); }
    CHECK(baud_current() == 2000000u && set_calls == 1);
    /* тишина: 4.99 с — держим, 5 с — назад на 600 000 */
    uint32_t f = 100;
    baud_poll(20000, f);
    baud_poll(24999, f);
    CHECK(baud_current() == 2000000u);
    baud_poll(25000, f);
    CHECK(baud_current() == 600000u && set_calls == 2 && set_last == 600000u);
    baud_poll(40000, f);
    CHECK(set_calls == 2);                 /* на 600 000 сторож молчит */
    /* отсчёт и от момента переключения: кадров после -baud нет */
    reset();
    baud_poll(0, 0);
    h("-baud 1000000");
    baud_poll(100, 1);
    baud_poll(5099, 1);
    CHECK(baud_current() == 1000000u);
    baud_poll(5100, 1);
    CHECK(baud_current() == 600000u);
    /* переполнение millis */
    reset();
    baud_poll(0xFFFFF000u, 0);
    h("-baud 1500000");
    baud_poll(0xFFFFF000u, 1);
    baud_poll(1000u, 1);                   /* прошло 5096 мс */
    CHECK(baud_current() == 600000u);
}

int main(void)
{
    test_parse();
    test_switch_after_reply();
    test_watchdog();
    return T_DONE();
}
