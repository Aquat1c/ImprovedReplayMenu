#pragma once

#include <cstdint>

// Device resets are coordinated through the exported EFZ_QueryOverlayResetApiV1.
namespace replay::render::d3d9
{
enum class TextAlign : std::uint8_t
{
    Left = 0,
    Center,
    Right,
};

struct TextItem
{
    std::int16_t x0 = 0;      // lane bounds, 320x240 native space
    std::int16_t x1 = 0;
    std::int16_t y = 0;       // lane top
    std::int16_t h = 12;      // lane height; text is centred vertically in it
    std::int16_t clipX0 = 0;  // clipX1 > clipX0 enables horizontal clipping
    std::int16_t clipX1 = 0;
    TextAlign align = TextAlign::Center;
    std::uint32_t argb = 0xFFFFFFFFu;
    std::uint32_t fillArgb = 0; // nonzero: fill the lane [x0,x1]x[y,y+h) under the text
    bool smallText = false;
    bool noScroll = false;      // text wider than [x0,x1] is cut there instead of scrolling
    // Nonzero: draw the game's 5x7 bitmap font instead, `pixelScale` RT pixels per dot,
    // with a 1-dot dark shadow; x0/x1/y are then 640x480 RT pixels (centred in [x0,x1]).
    float pixelScale = 0.0f;
    char text[128] = {};
};

bool EnsureHookInstalled();

bool IsAvailable();

// Game-thread frame protocol: Begin, Submit*, Commit. An empty commit stops drawing.
void BeginFrame();
void Submit(const TextItem& item);
void CommitFrame();
void Clear();

void Shutdown();
}
