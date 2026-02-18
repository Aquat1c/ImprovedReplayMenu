#pragma once

#include <cstdint>
#include <cstring>

namespace replay::input
{
enum class KeyEvent : std::uint8_t { None, Press, HoldStart, HoldEnd };

struct KeyTimer
{
    bool down = false;
    bool holding = false;
    std::uint32_t t0 = 0;
};

inline KeyEvent StepKey(KeyTimer& k, bool down, std::uint32_t now, std::uint32_t holdMs)
{
    if (down && !k.down)
    {
        k.down = true;
        k.t0 = now;
        k.holding = false;
        return KeyEvent::Press;
    }
    if (!down && k.down)
    {
        const bool held = k.holding;
        k.down = false;
        k.holding = false;
        return held ? KeyEvent::HoldEnd : KeyEvent::None;
    }
    if (down && !k.holding && now - k.t0 >= holdMs)
    {
        k.holding = true;
        return KeyEvent::HoldStart;
    }
    return KeyEvent::None;
}

inline void ArmKey(KeyTimer& k, bool down)
{
    k.down = down;
    k.holding = down;
    k.t0 = 0;
}

struct KeyCapture
{
    bool active = false;
    bool primed = false;
    bool prev[256] = {};
};

inline void BeginKeyCapture(KeyCapture& c, const bool (&down)[256])
{
    c.active = true;
    c.primed = false;
    std::memcpy(c.prev, down, sizeof(c.prev));
}

inline int StepKeyCapture(KeyCapture& c, const bool (&down)[256], bool (*excluded)(int))
{
    bool anyHeld = false;
    int got = 0;
    for (int k = 1; k < 256; ++k)
    {
        const bool edge = down[k] && !c.prev[k];
        c.prev[k] = down[k];
        if (excluded(k))
        {
            continue;
        }
        anyHeld = anyHeld || down[k];
        if (c.primed && edge && got == 0)
        {
            got = k;
        }
    }
    if (!c.primed)
    {
        if (!anyHeld)
        {
            c.primed = true;
        }
        return 0;
    }
    return got;
}
}
