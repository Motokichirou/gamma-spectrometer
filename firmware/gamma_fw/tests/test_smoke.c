#include "t.h"
#include "ccm.h"

int main(void)
{
    CHECK(1 + 1 == 2);
    CHECK_NEAR(0.1 + 0.2, 0.3, 1e-9);
    return T_DONE();
}
