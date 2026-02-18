#include "replay/core/key_capture.h"

#include <cstdio>

using namespace replay::input;

namespace
{
int g_failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); ++g_failures; } } while (0)

bool Excluded(int dik)
{
    return dik == 1;
}

void TestCapture()
{
    bool down[256] = {};
    KeyCapture c;
    down[0x2C] = true;
    BeginKeyCapture(c, down);
    CHECK(StepKeyCapture(c, down, &Excluded) == 0);
    down[0x2D] = true;
    CHECK(StepKeyCapture(c, down, &Excluded) == 0);
    down[0x2C] = down[0x2D] = false;
    CHECK(StepKeyCapture(c, down, &Excluded) == 0);
    CHECK(c.primed);
    down[0x2C] = true;
    CHECK(StepKeyCapture(c, down, &Excluded) == 0x2C);

    bool down2[256] = {};
    KeyCapture e;
    BeginKeyCapture(e, down2);
    down2[1] = true;
    CHECK(StepKeyCapture(e, down2, &Excluded) == 0);
    CHECK(e.primed);
    down2[1] = false;
    CHECK(StepKeyCapture(e, down2, &Excluded) == 0);
    down2[1] = true;
    CHECK(StepKeyCapture(e, down2, &Excluded) == 0);

    down2[1] = false;
    (void)StepKeyCapture(e, down2, &Excluded);
    down2[0x30] = down2[0x20] = true;
    CHECK(StepKeyCapture(e, down2, &Excluded) == 0x20);
    down2[0x30] = down2[0x20] = false;
    (void)StepKeyCapture(e, down2, &Excluded);
    down2[0x20] = true;
    CHECK(StepKeyCapture(e, down2, &Excluded) == 0x20);
}

void TestTiming()
{
    KeyTimer k;
    CHECK(StepKey(k, true, 1000, 250) == KeyEvent::Press);
    CHECK(StepKey(k, true, 1249, 250) == KeyEvent::None);
    CHECK(StepKey(k, true, 1250, 250) == KeyEvent::HoldStart);
    CHECK(StepKey(k, true, 1300, 250) == KeyEvent::None);
    CHECK(StepKey(k, false, 1400, 250) == KeyEvent::HoldEnd);
    CHECK(StepKey(k, true, 2000, 250) == KeyEvent::Press);
    CHECK(StepKey(k, false, 2010, 250) == KeyEvent::None);

    KeyTimer armed;
    ArmKey(armed, true);
    CHECK(StepKey(armed, true, 5000, 250) == KeyEvent::None);
    (void)StepKey(armed, false, 5100, 250);
    CHECK(StepKey(armed, true, 5200, 250) == KeyEvent::Press);
}
}

int main()
{
    TestCapture();
    TestTiming();
    if (g_failures == 0)
    {
        std::puts("key_capture_tests: OK");
    }
    return g_failures == 0 ? 0 : 1;
}
