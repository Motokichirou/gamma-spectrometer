/* Мини-фреймворк тестов: CHECK, CHECK_NEAR, T_DONE. */
#ifndef T_H
#define T_H
#include <stdio.h>
#include <math.h>

static int t_pass_, t_fail_;

#define CHECK(cond) do { \
    if (cond) { t_pass_++; } \
    else { t_fail_++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_NEAR(a, b, eps) do { \
    double t_a_ = (double)(a), t_b_ = (double)(b), t_e_ = (double)(eps); \
    if (fabs(t_a_ - t_b_) <= t_e_) { t_pass_++; } \
    else { t_fail_++; printf("  FAIL %s:%d: %s ~ %s (%.9g vs %.9g, eps %g)\n", \
                             __FILE__, __LINE__, #a, #b, t_a_, t_b_, t_e_); } \
} while (0)

/* сбой — и упавшие проверки, и ни одной проверки (тест, который ничего не проверил, — не зелёный) */
#define T_DONE() (printf("%d passed, %d failed%s\n", t_pass_, t_fail_, \
                         t_pass_ + t_fail_ ? "" : " — НИ ОДНОЙ ПРОВЕРКИ"), \
                  (t_fail_ || !t_pass_) ? 1 : 0)

#endif
