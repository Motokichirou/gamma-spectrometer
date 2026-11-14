#include "t.h"
#include "ccm.h"

int main(void)
{
    /* T_RESULT (как T_DONE, но без печати): ни одной проверки — сбой; только прошедшие — успех; есть упавшая — сбой */
    int r0 = T_RESULT();
    t_pass_ = 3;
    int r1 = T_RESULT();
    t_fail_ = 1;
    int r2 = T_RESULT();
    t_pass_ = t_fail_ = 0;
    CHECK(r0 == 1);
    CHECK(r1 == 0);
    CHECK(r2 == 1);
    CHECK(1 + 1 == 2);
    CHECK_NEAR(0.1 + 0.2, 0.3, 1e-9);
    return T_DONE();
}
