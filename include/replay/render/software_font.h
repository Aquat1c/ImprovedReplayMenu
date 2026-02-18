#pragma once

#include <cstdint>
#include <string>

namespace replay::render
{
struct IndexedSurfaceView
{
    std::uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    int pitch = 0;
};

// 7 rows of 5-bit masks (bit 4 = leftmost column), or nullptr for unsupported chars.
const std::uint8_t* Glyph5x7(char c);

void DrawTextCentered5x7(
    const IndexedSurfaceView& surface,
    const std::string& text,
    int leftX,
    int rightX,
    int y,
    int scaleX,
    int scaleY,
    std::uint8_t color);
}
