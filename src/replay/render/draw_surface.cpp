#include "replay/render/draw_surface.h"

#include "logger.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <windows.h>

namespace
{
constexpr std::uint32_t kOffsetWindowHandle = 8;
constexpr std::uint32_t kOffsetGraphicsContext = 32;
constexpr std::uint32_t kOffsetPalette = 46;
constexpr std::uint32_t kOffsetTransparentColor = 1070;
constexpr std::uint32_t kOffsetGraphicsPrimarySurface = 33283u * sizeof(std::uint32_t);
constexpr std::uint32_t kOffsetGraphicsBackBufferSurface = 33284u * sizeof(std::uint32_t);

constexpr std::uint32_t kVtableOffsetSurfaceLock = 100;
constexpr std::uint32_t kVtableOffsetSurfaceUnlock = 128;

struct DrawLogState
{
    bool surfaceLockOk = false;
    bool surfaceLockUnavailable = false;
    bool backbufferSurfaceDesc = false;
    bool primarySurfaceDesc = false;
    bool overlayPaletteChoice = false;
};

DrawLogState g_drawLogState = {};

bool IsExecutableAddress(std::uint32_t address)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == 0)
    {
        return false;
    }
    if (mbi.State != MEM_COMMIT)
    {
        return false;
    }

    const DWORD protect = mbi.Protect & 0xFF;
    return protect == PAGE_EXECUTE
        || protect == PAGE_EXECUTE_READ
        || protect == PAGE_EXECUTE_READWRITE
        || protect == PAGE_EXECUTE_WRITECOPY;
}

bool QuerySurfaceDimensions(void* surface, int* outWidth, int* outHeight)
{
    if (surface == nullptr || outWidth == nullptr || outHeight == nullptr)
    {
        return false;
    }

    const std::uint32_t vtable = *reinterpret_cast<std::uint32_t*>(surface);
    if (vtable == 0)
    {
        return false;
    }

    constexpr std::uint32_t kVtableOffsetSurfaceGetDesc = 88;
    const std::uint32_t getDescAddress = *reinterpret_cast<std::uint32_t*>(vtable + kVtableOffsetSurfaceGetDesc);
    if (getDescAddress == 0 || !IsExecutableAddress(getDescAddress))
    {
        return false;
    }

    auto const getDesc = reinterpret_cast<HRESULT(__stdcall*)(void*, std::uint32_t*)>(getDescAddress);
    std::uint32_t desc[36] = {};
    desc[0] = 124;
    const HRESULT hr = getDesc(surface, desc);
    if (!SUCCEEDED(hr))
    {
        return false;
    }

    const int width = static_cast<int>(desc[3]);
    const int height = static_cast<int>(desc[2]);
    if (width <= 0 || height <= 0)
    {
        return false;
    }

    *outWidth = width;
    *outHeight = height;
    return true;
}

bool TryLockMenuSurface(void* surface, const char* label, replay::render::LockedMenuSurface* outSurface)
{
    if (surface == nullptr || outSurface == nullptr)
    {
        return false;
    }

    const std::uint32_t vtable = *reinterpret_cast<std::uint32_t*>(surface);
    if (vtable == 0)
    {
        return false;
    }
    const std::uint32_t lockAddress = *reinterpret_cast<std::uint32_t*>(vtable + kVtableOffsetSurfaceLock);
    if (lockAddress == 0 || !IsExecutableAddress(lockAddress))
    {
        return false;
    }

    auto const lockSurface = reinterpret_cast<HRESULT(__stdcall*)(void*, void*, std::uint32_t*, int, std::uint32_t)>(lockAddress);
    auto const unlockSurface =
        reinterpret_cast<HRESULT(__stdcall*)(void*, void*)>(*reinterpret_cast<std::uint32_t*>(vtable + kVtableOffsetSurfaceUnlock));
    std::uint32_t lockDesc[36] = {};
    lockDesc[0] = 124;
    const HRESULT hr = lockSurface(surface, nullptr, lockDesc, 1, 0);
    if (SUCCEEDED(hr))
    {
        const std::uintptr_t ptr9 = static_cast<std::uintptr_t>(lockDesc[9]);
        const std::uintptr_t ptr8 = static_cast<std::uintptr_t>(lockDesc[8]);
        const std::uintptr_t pixelPtr = (ptr9 != 0u) ? ptr9 : ptr8;
        const int lockWidth = static_cast<int>(lockDesc[3]);
        const int lockHeight = static_cast<int>(lockDesc[2]);
        const std::uint32_t descFlags = lockDesc[1];
        const std::uint32_t pixelFormatFlags = lockDesc[20];
        const int bitsPerPixel = static_cast<int>(lockDesc[22]);
        int width = lockWidth;
        int height = lockHeight;
        const int pitch = static_cast<int>(lockDesc[4]);

        int descWidth = 0;
        int descHeight = 0;
        if ((width <= 0 || height <= 0) && QuerySurfaceDimensions(surface, &descWidth, &descHeight))
        {
            width = (width > 0) ? width : descWidth;
            height = (height > 0) ? height : descHeight;
        }

        if (width <= 0 && pitch > 0 && bitsPerPixel >= 8 && (bitsPerPixel % 8) == 0)
        {
            const int bytesPerPixel = bitsPerPixel / 8;
            if (bytesPerPixel > 0 && (pitch % bytesPerPixel) == 0)
            {
                width = pitch / bytesPerPixel;
            }
        }
        if (width <= 0 && pitch > 0)
        {
            width = pitch;
        }
        if (height <= 0)
        {
            height = 240;
        }

        if (pixelPtr == 0u || pitch <= 0 || width <= 0 || height <= 0)
        {
            if (unlockSurface != nullptr && IsExecutableAddress(reinterpret_cast<std::uint32_t>(unlockSurface)))
            {
                (void)unlockSurface(surface, reinterpret_cast<void*>(pixelPtr));
            }
            if (!g_drawLogState.surfaceLockUnavailable)
            {
                mod::Log(
                    "ReplayDrawSurface::AcquireLock: %s payload invalid surface=0x%08X ptr9=0x%08X ptr8=0x%08X pitch=%u w=%d h=%d",
                    label,
                    static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(surface)),
                    static_cast<unsigned>(ptr9),
                    static_cast<unsigned>(ptr8),
                    static_cast<unsigned>(lockDesc[4]),
                    width,
                    height);
            }
            return false;
        }

        outSurface->surface = surface;
        outSurface->width = width;
        outSurface->height = height;
        outSurface->pitch = pitch;
        outSurface->lockWidth = lockWidth;
        outSurface->lockHeight = lockHeight;
        outSurface->descFlags = descFlags;
        outSurface->pixelFormatFlags = pixelFormatFlags;
        outSurface->bitsPerPixel = bitsPerPixel;
        outSurface->pixels = reinterpret_cast<std::uint8_t*>(pixelPtr);
        outSurface->lockToken = reinterpret_cast<void*>(pixelPtr);
        outSurface->label = label;
        if (!g_drawLogState.surfaceLockOk)
        {
            mod::Log(
                "ReplayDrawSurface::AcquireLock: locked %s surface=0x%08X size=%dx%d pitch=%d lockW=%d lockH=%d bpp=%d descFlags=0x%08X pfFlags=0x%08X",
                label,
                static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(surface)),
                outSurface->width,
                outSurface->height,
                outSurface->pitch,
                outSurface->lockWidth,
                outSurface->lockHeight,
                outSurface->bitsPerPixel,
                static_cast<unsigned>(outSurface->descFlags),
                static_cast<unsigned>(outSurface->pixelFormatFlags));
            g_drawLogState.surfaceLockOk = true;
        }
        if ((std::strcmp(label, "backbuffer") == 0 && !g_drawLogState.backbufferSurfaceDesc)
            || (std::strcmp(label, "primary") == 0 && !g_drawLogState.primarySurfaceDesc))
        {
            mod::Log(
                "ReplayDrawSurface::AcquireLock: %s details size=%dx%d pitch=%d lockW=%d lockH=%d bpp=%d descFlags=0x%08X pfFlags=0x%08X",
                label,
                outSurface->width,
                outSurface->height,
                outSurface->pitch,
                outSurface->lockWidth,
                outSurface->lockHeight,
                outSurface->bitsPerPixel,
                static_cast<unsigned>(outSurface->descFlags),
                static_cast<unsigned>(outSurface->pixelFormatFlags));
            if (std::strcmp(label, "backbuffer") == 0)
            {
                g_drawLogState.backbufferSurfaceDesc = true;
            }
            else if (std::strcmp(label, "primary") == 0)
            {
                g_drawLogState.primarySurfaceDesc = true;
            }
        }
        return true;
    }

    if (!g_drawLogState.surfaceLockUnavailable)
    {
        mod::Log(
            "ReplayDrawSurface::AcquireLock: %s Lock failed surface=0x%08X hr=0x%08X",
            label,
            static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(surface)),
            static_cast<unsigned>(hr));
    }
    return false;
}
}

namespace replay::render
{
bool AcquireMenuDrawSurfaceLock(std::uint32_t screenContext, LockedMenuSurface* outSurface)
{
    if (outSurface == nullptr)
    {
        return false;
    }

    *outSurface = {};
    const std::uint32_t graphicsContext = *reinterpret_cast<std::uint32_t*>(screenContext + kOffsetGraphicsContext);
    if (graphicsContext == 0)
    {
        mod::Log("ReplayDrawSurface::AcquireLock: graphicsContext null");
        return false;
    }

    void* backBufferSurface = *reinterpret_cast<void**>(graphicsContext + kOffsetGraphicsBackBufferSurface);
    if (TryLockMenuSurface(backBufferSurface, "backbuffer", outSurface))
    {
        return true;
    }

    void* primarySurface = *reinterpret_cast<void**>(graphicsContext + kOffsetGraphicsPrimarySurface);
    if (TryLockMenuSurface(primarySurface, "primary", outSurface))
    {
        return true;
    }

    if (!g_drawLogState.surfaceLockUnavailable)
    {
        mod::Log("ReplayDrawSurface::AcquireLock: no lockable menu surface");
        g_drawLogState.surfaceLockUnavailable = true;
    }
    return false;
}

void ReleaseMenuDrawSurfaceLock(const LockedMenuSurface& surface)
{
    if (surface.surface == nullptr || surface.lockToken == nullptr)
    {
        return;
    }
    const std::uint32_t vtable = *reinterpret_cast<std::uint32_t*>(surface.surface);
    if (vtable == 0)
    {
        return;
    }
    const std::uint32_t unlockAddress = *reinterpret_cast<std::uint32_t*>(vtable + kVtableOffsetSurfaceUnlock);
    if (unlockAddress == 0 || !IsExecutableAddress(unlockAddress))
    {
        return;
    }
    auto const unlockSurface = reinterpret_cast<HRESULT(__stdcall*)(void*, void*)>(unlockAddress);
    (void)unlockSurface(surface.surface, surface.lockToken);
}

void ResolveOverlayTextPaletteColors(
    std::uint32_t screenContext,
    std::uint8_t paletteStart,
    std::uint8_t paletteCount,
    std::uint8_t* outSelected,
    std::uint8_t* outNormal)
{
    if (outSelected == nullptr || outNormal == nullptr || paletteCount == 0)
    {
        return;
    }

    const std::uint8_t transparentColor = *reinterpret_cast<std::uint8_t*>(screenContext + kOffsetTransparentColor);
    const std::uint8_t* palette = reinterpret_cast<std::uint8_t*>(screenContext + kOffsetPalette);

    int bestIndex = -1;
    int bestScore = -0x7FFFFFFF;
    for (int i = 0; i < paletteCount; ++i)
    {
        const int idx = static_cast<int>(paletteStart) + i;
        if (idx < 0 || idx >= 256 || idx == static_cast<int>(transparentColor))
        {
            continue;
        }
        const int r = palette[idx * 4 + 0];
        const int g = palette[idx * 4 + 1];
        const int b = palette[idx * 4 + 2];
        const int luma = 30 * r + 59 * g + 11 * b;
        const int cmax = (std::max)((std::max)(r, g), b);
        const int cmin = (std::min)((std::min)(r, g), b);
        const int saturation = cmax - cmin;
        const int score = luma - saturation * 220;
        if (score > bestScore)
        {
            bestScore = score;
            bestIndex = idx;
        }
    }

    if (bestIndex < 0)
    {
        *outSelected = paletteStart;
        *outNormal = static_cast<std::uint8_t>(paletteStart + ((paletteCount > 1) ? (paletteCount - 1) : 0));
        return;
    }

    const int selR = palette[bestIndex * 4 + 0];
    const int selG = palette[bestIndex * 4 + 1];
    const int selB = palette[bestIndex * 4 + 2];
    const int selectedLuma = 30 * selR + 59 * selG + 11 * selB;
    const int targetLuma = (selectedLuma * 65) / 100;
    int normalIndex = -1;
    int bestDelta = 0x7FFFFFFF;
    for (int i = 0; i < paletteCount; ++i)
    {
        const int idx = static_cast<int>(paletteStart) + i;
        if (idx < 0 || idx >= 256 || idx == static_cast<int>(transparentColor) || idx == bestIndex)
        {
            continue;
        }
        const int r = palette[idx * 4 + 0];
        const int g = palette[idx * 4 + 1];
        const int b = palette[idx * 4 + 2];
        const int luma = 30 * r + 59 * g + 11 * b;
        const int cmax = (std::max)((std::max)(r, g), b);
        const int cmin = (std::min)((std::min)(r, g), b);
        const int saturation = cmax - cmin;
        const int delta = std::abs(luma - targetLuma) + saturation * 8;
        if (delta < bestDelta)
        {
            bestDelta = delta;
            normalIndex = idx;
        }
    }

    if (normalIndex < 0)
    {
        normalIndex = bestIndex;
    }

    *outSelected = static_cast<std::uint8_t>(bestIndex);
    *outNormal = static_cast<std::uint8_t>(normalIndex);

    if (!g_drawLogState.overlayPaletteChoice)
    {
        const int nr = palette[normalIndex * 4 + 0];
        const int ng = palette[normalIndex * 4 + 1];
        const int nb = palette[normalIndex * 4 + 2];
        mod::Log(
            "ReplayDrawSurface::Palette: selected=%u rgb=(%d,%d,%d) normal=%u rgb=(%d,%d,%d) transparent=%u",
            static_cast<unsigned>(*outSelected),
            selR,
            selG,
            selB,
            static_cast<unsigned>(*outNormal),
            nr,
            ng,
            nb,
            static_cast<unsigned>(transparentColor));
        g_drawLogState.overlayPaletteChoice = true;
    }
}
}
