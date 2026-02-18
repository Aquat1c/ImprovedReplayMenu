#pragma once

#include <cmath>
#include <cstdint>

// Vanilla's replay menu motion (updateReplayScreenLogic states 1 and 3, renderReplayScreen), in 320x240 Y.
// The angle runs 0 (list at rest) to 180 (rows gone, panel up).
namespace replay::motion
{
inline double Cosine(double angle)
{
    return std::cos(angle / 57.29579143313326);
}

// Added to every unselected row. Truncated as vanilla does, so text stays on the bars it draws.
inline int RowDrop(double angle)
{
    return static_cast<int>(160.0 - Cosine(angle) * 160.0);
}

inline double SelectedRise(int row, double angle)
{
    return (96.0 + 16.0 + row * 16.0) * angle / 180.0;
}

inline float TextScrollOffset(float overflow, std::uint32_t elapsedMs)
{
    constexpr std::uint32_t kRestMs = 1200;
    constexpr float kPixelsPerMs = 0.06f;
    const std::uint32_t runMs = static_cast<std::uint32_t>(overflow / kPixelsPerMs);
    const std::uint32_t t = elapsedMs % (kRestMs + runMs + kRestMs);
    if (t < kRestMs)
    {
        return 0.0f;
    }
    const float offset = (t - kRestMs) * kPixelsPerMs;
    return offset < overflow ? offset : overflow;
}

inline int PanelOffset(double angle)
{
    return static_cast<int>(std::lround(160.0 + Cosine(angle) * 160.0));
}
}
