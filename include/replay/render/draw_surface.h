#pragma once

#include <cstdint>

namespace replay::render
{
struct LockedMenuSurface
{
    void* surface = nullptr;
    std::uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    int pitch = 0;
    int lockWidth = 0;
    int lockHeight = 0;
    std::uint32_t descFlags = 0;
    std::uint32_t pixelFormatFlags = 0;
    int bitsPerPixel = 0;
    void* lockToken = nullptr;
    const char* label = "";
};

bool AcquireMenuDrawSurfaceLock(std::uint32_t screenContext, LockedMenuSurface* outSurface);
void ReleaseMenuDrawSurfaceLock(const LockedMenuSurface& surface);

void ResolveOverlayTextPaletteColors(
    std::uint32_t screenContext,
    std::uint8_t paletteStart,
    std::uint8_t paletteCount,
    std::uint8_t* outSelected,
    std::uint8_t* outNormal);
}
