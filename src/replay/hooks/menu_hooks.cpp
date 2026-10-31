#include "replay/hooks/menu_hooks.h"
#include "replay/core/replay_name_format.h"

#include "crash_handler.h"
#include "logger.h"
#include "mod_version.h"
#include "replay/core/i18n.h"
#include "replay/core/menu_motion.h"
#include "replay/core/replay_browser.h"
#include "replay/core/settings.h"
#include "replay/core/patch_utils.h"
#include "replay/hooks/replay_flow.h"
#include "replay/hooks/rewind.h"
#include "replay/render/d3d9_overlay.h"
#include "replay/render/draw_surface.h"
#include "replay/render/software_font.h"

#include "MinHook.h"
#include "replay/core/hook_install.h"

#include <algorithm>
#include <iterator>
#include <atomic>
#include <cmath>
#include <climits>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace
{
constexpr std::uintptr_t kImageBase = 0x00400000;
constexpr std::uintptr_t kVaOpenFileReader = 0x00405E90;
constexpr std::uintptr_t kVaProcessPlayerInput = 0x00406590;
constexpr std::uintptr_t kVaPlaySoundEffect = 0x00406860;
constexpr std::uintptr_t kVaPresentFrameToScreen = 0x0040B5E0;
constexpr std::uintptr_t kVaUpdateReplayScreenLogic = 0x0076F060;
constexpr std::uintptr_t kVaRenderReplayScreen = 0x0076F780;
constexpr std::uintptr_t kVaFadeWithSoundAdjustment = 0x00759C90;
constexpr std::uintptr_t kVaCheckReplayExitInput = 0x00770D80;
constexpr std::uintptr_t kVaReplayVtableRender = 0x007897DC;
constexpr std::uintptr_t kVaReplayVtableUpdate = 0x007897E0;
constexpr std::uintptr_t kVaUpdateBattleScreenLogic = 0x00763C20;
constexpr std::uintptr_t kVaBattleVtableUpdate = 0x007895E0;
constexpr std::uintptr_t kVaBlitSurfaceWithTransparency = 0x00409A90;
// renderReplayScreen's first call: the full-screen replay_bg blit (surface screen+1076).
constexpr std::uintptr_t kVaReplayBackgroundBlitCall = 0x0076F874;
constexpr double kReplayBackgroundScrollPixelsPerSecond = 12.5;

constexpr std::uint32_t kOffsetGraphicsContext = 32;
constexpr std::uint32_t kGraphicsPaletteOffset = 33285 * 4; // IDirectDrawPalette* (GetEntries = vtbl+16)
constexpr std::uint32_t kOffsetSystemPtr = 28;
constexpr std::uint32_t kOffsetFirstTimeFlag = 44;
constexpr std::uint32_t kOffsetExitFlag = 45;
constexpr std::uint32_t kSysReplayIoMode = 82563;     // 1 = playback
constexpr std::uint32_t kSysReplayFileReader = 82564; // reader object; dword [0] = EOF flag
constexpr std::uint32_t kSysGameMode = 4964;           // byte; 5 = replay (set by the title menu)
constexpr std::uint8_t kGameModeReplay = 5;
constexpr int kIndicatorCenterX = 320;
constexpr int kIndicatorTopY = 432;
constexpr float kIndicatorDotSize = 1.14f;
constexpr std::uint32_t kOffsetBattlePaused = 1416;    // battle dword; nonzero = paused (update returns 3)
constexpr std::uint32_t kOffsetMenuState = 1129;
constexpr std::uint32_t kOffsetSelectedRow = 1130;
constexpr std::uint32_t kOffsetAnimPhase = 1864;
constexpr std::uint32_t kOffsetAnimRowOffset = 1872;
constexpr std::uint32_t kOffsetLegacyExistsGrid = 1134; // 9 * 9
constexpr std::uint32_t kOffsetPalette = 46;
constexpr std::uint32_t kOffsetReplayTransparentColor = 1070;
constexpr std::uint32_t kOffsetReplayUiSurface = 1080;
constexpr std::uint32_t kVtableOffsetSurfaceLock = 100;
constexpr std::uint32_t kVtableOffsetSurfaceUnlock = 128;

std::mutex g_hookMutex;
std::atomic<bool> g_hooksInstalled{false};
std::uintptr_t g_exeBase = 0;
constexpr bool kUseDllLocalReplayAssets = false;

// MinHook detours (thread-freezing, relocating). Patches below are plain byte writes.
void* g_openFileReaderTarget = nullptr;
void* g_presentFrameToScreenTarget = nullptr;
std::vector<replay::patch::PatchRecord> g_appliedPatches;

using RenderReplayScreenFn = BOOL(__thiscall*)(std::uint32_t screenContext);
using UpdateReplayScreenLogicFn = char(__thiscall*)(std::uint32_t screenContext);
using OpenFileReaderFn = int(__thiscall*)(void* fileReaderStruct, const char* filePath);
using ProcessPlayerInputFn = void(__thiscall*)(int* inputManager);
using PlaySoundEffectFn = int(__thiscall*)(HWND** soundManager, unsigned short soundIndex);
using FadeWithSoundAdjustmentFn = int(__thiscall*)(void* screenContext, int paletteId, std::uint8_t fadeDirection, int baseVolume, int volumeAdjustment);
using CheckReplayExitInputFn = int(__thiscall*)(std::uint32_t screenContext);
using PresentFrameToScreenFn = BOOL(__thiscall*)(std::uint32_t graphicsContext);
using BlitSurfaceWithTransparencyFn = BOOL(__thiscall*)(void* graphicsContext, int destX, int destY, int destRight,
    int destBottom, int sourceSurface, int srcX, int srcY, int srcRight, int srcBottom, char transparentColor,
    int flipHorizontal);
RenderReplayScreenFn g_originalRenderReplayScreen = nullptr;
UpdateReplayScreenLogicFn g_originalUpdateReplayScreenLogic = nullptr;
UpdateReplayScreenLogicFn g_originalUpdateBattleScreenLogic = nullptr; // same char(__thiscall*)(screen)
OpenFileReaderFn g_originalOpenFileReader = nullptr;
ProcessPlayerInputFn g_processPlayerInput = nullptr;
PlaySoundEffectFn g_playSoundEffect = nullptr;
FadeWithSoundAdjustmentFn g_fadeWithSoundAdjustment = nullptr;
CheckReplayExitInputFn g_checkReplayExitInput = nullptr;
PresentFrameToScreenFn g_originalPresentFrameToScreen = nullptr;

bool g_insideReplayRender = false;
std::uint32_t g_playbackFrame = 0;
DWORD g_playbackTick = 0;
std::uint32_t g_playbackBattle = 0;
bool DrawPlaybackFrameIndicator();
bool ReadRevivalReplayFrame(std::uint32_t* outFrame, bool* outPaused = nullptr);
double g_replayBackgroundScrollOffset = 0.0;
DWORD g_replayBackgroundScrollTick = 0;
std::uint32_t g_activeReplayRenderScreenContext = 0;
std::uint64_t g_replayRenderFrameCounter = 0;
DWORD g_lastReplayLogicBeginTick = 0;
DWORD g_lastReplayLogicIdleTick = 0;
DWORD g_lastOverlayRecoveryTick = 0;
DWORD g_lastOverlayLayoutTick = 0;
DWORD g_lastOverlayRowDiagTick = 0;
constexpr const char* kHookBuildTag = "directplay-v3-verbose-xyfix-yup3-noswap-sheet-trim-visible-rows-v3-custom-horizontal-dmenu-sprite-slotreuse-v1-unicodeopen-v1-recordsafe-v1-cjkfont-v1-mixed-v1";
constexpr int kOverlayBaseYNative = 96;
constexpr int kOverlayRowStepNative = 16;
constexpr int kOverlayRowHeightNative = 16;
constexpr int kOverlayLeftXNative = 8;
constexpr int kOverlayRightXNative = 312;
constexpr int kOverlayTextNativeNudgeY = -3;
constexpr int kOverlayTextLaneHeightNative = 12;
constexpr int kOverlayNonJapaneseLiftNative = -2;
constexpr bool kOverlaySwapSelectedStyling = false;
constexpr bool kOverlayVerboseSampling = true;
constexpr bool kOverlayLogRowDiagnosticsAlways = false;
constexpr int kOverlayVisibleRowsMax = 9;
constexpr bool kOverlayRewriteRowBackgrounds = false;
constexpr std::uint8_t kOverlayTextClearColorIndex = 0;
constexpr int kOptionsPanelLeftNative = 64;
constexpr int kOptionsPanelRightNative = 256;
constexpr int kOptionsPanelTopNative = 14;
constexpr int kOptionsTitleHeightNative = 18;
constexpr int kOptionsItemStartYNative = 36;
constexpr int kOptionsItemStepNative = 14;
constexpr int kOptionsHighlightHeightNative = 12;
constexpr int kOptionsAboutYNative = 150;
constexpr int kOptionsAboutLineNative = 10;
constexpr int kOptionsPanelBottomNative = 182;
constexpr int kOptionsTextPadNative = 8;
constexpr int kOptionsTipLeftNative = 16;
constexpr int kOptionsTipRightNative = 304;
constexpr int kOptionsTipTopNative = 208;
constexpr int kOptionsTipLineNative = 11;
constexpr int kOptionsTipBottomNative = kOptionsTipTopNative + 2 * kOptionsTipLineNative + 3;
constexpr std::size_t kOptionsItemCount = static_cast<std::size_t>(replay::hooks::OptionsItem::Count);
constexpr int kRtTextLaneHeightNative = 12;
constexpr std::uint32_t kRtTextSelectedArgb = 0xFFFFFFFFu;
constexpr std::uint32_t kRtTextNormalArgb = 0xFFBEBEBEu;
constexpr std::uint32_t kRtTextOnHighlightArgb = 0xFF101010u;
constexpr std::uint32_t kRtSelectedBarWashArgb = 0x38FFFFFFu;
constexpr std::uint32_t kRtTextHintArgb = 0xFF8C8C8Cu;

// Replay DAT atlas mapping (matches generator layout and vanilla replay sheet structure).
constexpr int kDatLaneX0 = 8;
constexpr int kDatLaneX1 = 312;
constexpr int kDatSelectedRowsBaseY = 92;
constexpr int kDatUnselectedRowsBaseY = 252;
constexpr int kDatRowHeight = 16;
constexpr int kReplayOptionRowCount = 6;
constexpr int kReplayOptionRowSources[kReplayOptionRowCount] = {716, 732, 748, 852, 868, 884};

struct ReplayDatAtlas
{
    bool triedLoad = false;
    bool loaded = false;
    int width = 0;
    int height = 0;
    int paletteEntries = 0;
    std::size_t dimOffset = 0;
    std::size_t pixelOffset = 0;
    std::string formatTag;
    std::string sourcePath;
    std::uint8_t paletteRgb[256 * 3] = {};
    std::vector<std::uint8_t> pixelsTopDown;
};

struct ReplayRowLaneTrimState
{
    bool captured = false;
    bool optionRowsCaptured = false;
    void* surface = nullptr;
    int pitch = 0;
    int width = 0;
    int height = 0;
    std::size_t lastAppliedVisibleRows = static_cast<std::size_t>(-1);
    bool lastAppliedMoving = false;
    bool lastAppliedHideOptionRows = false;
    std::uint8_t selectedRows[kOverlayVisibleRowsMax][kDatRowHeight * 320] = {};
    std::uint8_t unselectedRows[kOverlayVisibleRowsMax][kDatRowHeight * 320] = {};
    std::uint8_t optionRows[kReplayOptionRowCount][kDatRowHeight * 320] = {};
};

ReplayDatAtlas g_replayDatAtlas = {};
ReplayRowLaneTrimState g_replayRowLaneTrimState = {};
bool g_replayDatPaletteMapLogged = false;
bool g_replayLogicStateInitialized = false;
std::uint8_t g_lastReplayLogicFirst = 0;
std::uint8_t g_lastReplayLogicExit = 0;
std::uint8_t g_lastReplayLogicMenuState = 0;

#if defined(_M_IX86)
extern "C" void HookedOpenFileReaderThunk();
extern "C" void HookedPresentFrameToScreenThunk();
extern "C" void HookedRenderReplayScreenThunk();
extern "C" void HookedUpdateReplayScreenLogicThunk();
extern "C" void HookedUpdateBattleScreenLogicThunk();
#endif

// Unload only. Removal restores our saved bytes, which would cut out any hook
// another mod stacked on top later, so the detour is just disabled.
void DisableMinHook(void** target)
{
    if (*target != nullptr)
    {
        (void)MH_DisableHook(*target);
        *target = nullptr;
    }
}

std::uintptr_t RuntimeAddress(std::uintptr_t va)
{
    return g_exeBase + (va - kImageBase);
}

bool ShouldLogEvery(DWORD* lastTick, DWORD intervalMs)
{
    if (lastTick == nullptr)
    {
        return true;
    }
    const DWORD now = GetTickCount();
    if (*lastTick == 0 || (now - *lastTick) >= intervalMs)
    {
        *lastTick = now;
        return true;
    }
    return false;
}

bool IsEfzRevivalLoaded()
{
    static int cached = -1;
    if (cached >= 0)
    {
        return cached != 0;
    }

    const HMODULE revivalA = GetModuleHandleA("EfzRevival.dll");
    const HMODULE revivalB = GetModuleHandleA("EFZRevival.dll");
    const HMODULE revivalC = GetModuleHandleA("efzrevival.dll");
    cached = (revivalA != nullptr || revivalB != nullptr || revivalC != nullptr) ? 1 : 0;
    mod::Log("ReplayHooks::IsEfzRevivalLoaded: detected=%d", cached);
    return cached != 0;
}

bool IsExecutableAddress(std::uintptr_t address)
{
    if (address == 0)
    {
        return false;
    }

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

std::uint8_t ReadByte(std::uint32_t base, std::uint32_t off)
{
    return *reinterpret_cast<std::uint8_t*>(base + off);
}

std::uint32_t ReadDword(std::uint32_t base, std::uint32_t off)
{
    return *reinterpret_cast<std::uint32_t*>(base + off);
}

void WriteByte(std::uint32_t base, std::uint32_t off, std::uint8_t value)
{
    *reinterpret_cast<std::uint8_t*>(base + off) = value;
}

std::uint8_t SampleIndexedSurfacePixel(const replay::render::IndexedSurfaceView& surface, int x, int y)
{
    if (surface.pixels == nullptr || x < 0 || y < 0 || x >= surface.width || y >= surface.height)
    {
        return 0;
    }
    return surface.pixels[y * surface.pitch + x];
}

std::uint32_t SampleRowChecksum(
    const replay::render::IndexedSurfaceView& surface,
    int leftX,
    int rightX,
    int topY,
    int rowHeight)
{
    if (surface.pixels == nullptr || surface.width <= 0 || surface.height <= 0 || rowHeight <= 0)
    {
        return 0u;
    }

    int clampedLeft = leftX;
    int clampedRight = rightX;
    int clampedTop = topY;
    int clampedBottom = topY + rowHeight - 1;
    if (clampedLeft < 0)
    {
        clampedLeft = 0;
    }
    if (clampedRight >= surface.width)
    {
        clampedRight = surface.width - 1;
    }
    if (clampedTop < 0)
    {
        clampedTop = 0;
    }
    if (clampedBottom >= surface.height)
    {
        clampedBottom = surface.height - 1;
    }
    if (clampedRight < clampedLeft || clampedBottom < clampedTop)
    {
        return 0u;
    }

    std::uint32_t checksum = 2166136261u;
    const int sampleStepX = 8;
    const int sampleStepY = 2;
    for (int y = clampedTop; y <= clampedBottom; y += sampleStepY)
    {
        for (int x = clampedLeft; x <= clampedRight; x += sampleStepX)
        {
            const std::uint8_t px = SampleIndexedSurfacePixel(surface, x, y);
            checksum ^= static_cast<std::uint32_t>(px);
            checksum *= 16777619u;
        }
    }
    return checksum;
}

bool TryLoadReplayDatAtlasFromPath(const char* path, ReplayDatAtlas* outAtlas)
{
    if (path == nullptr || outAtlas == nullptr)
    {
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        return false;
    }

    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 16u)
    {
        mod::Log("ReplayHooks::LoadReplayDatAtlas: '%s' too small (%u bytes)", path, static_cast<unsigned>(bytes.size()));
        return false;
    }

    auto tryDecodeAtOffsets = [&](int paletteEntries, std::size_t dimOffset, std::size_t pixelOffset, const char* tag) -> bool
    {
        if (paletteEntries <= 0 || paletteEntries > 256)
        {
            return false;
        }
        if (bytes.size() < dimOffset + 4u || pixelOffset < dimOffset + 4u || bytes.size() <= pixelOffset)
        {
            return false;
        }

        const int width = static_cast<int>(bytes[dimOffset + 0] | (static_cast<unsigned>(bytes[dimOffset + 1]) << 8));
        const int height = static_cast<int>(bytes[dimOffset + 2] | (static_cast<unsigned>(bytes[dimOffset + 3]) << 8));
        if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
        {
            return false;
        }

        const std::size_t pixelBytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        if (pixelBytes > (bytes.size() - pixelOffset))
        {
            return false;
        }

        outAtlas->width = width;
        outAtlas->height = height;
        outAtlas->paletteEntries = paletteEntries;
        outAtlas->dimOffset = dimOffset;
        outAtlas->pixelOffset = pixelOffset;
        outAtlas->formatTag = (tag != nullptr) ? tag : "unknown";
        outAtlas->sourcePath = path;
        std::memset(outAtlas->paletteRgb, 0, sizeof(outAtlas->paletteRgb));
        for (int i = 0; i < paletteEntries; ++i)
        {
            const std::size_t off = 1u + static_cast<std::size_t>(i) * 3u;
            // DAT stores palette triplets as BGR.
            outAtlas->paletteRgb[i * 3 + 0] = bytes[off + 2]; // R
            outAtlas->paletteRgb[i * 3 + 1] = bytes[off + 1]; // G
            outAtlas->paletteRgb[i * 3 + 2] = bytes[off + 0]; // B
        }
        outAtlas->pixelsTopDown.assign(pixelBytes, 0);

        // DAT payload is bottom-up rows.
        for (int y = 0; y < height; ++y)
        {
            const std::size_t srcRow = pixelOffset + static_cast<std::size_t>(height - 1 - y) * static_cast<std::size_t>(width);
            const std::size_t dstRow = static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
            std::memcpy(
                outAtlas->pixelsTopDown.data() + dstRow,
                bytes.data() + srcRow,
                static_cast<std::size_t>(width));
        }

        const std::size_t consumedBytes = pixelOffset + pixelBytes;
        const std::size_t trailingBytes = (bytes.size() > consumedBytes) ? (bytes.size() - consumedBytes) : 0u;
        mod::Log(
            "ReplayHooks::LoadReplayDatAtlas: loaded '%s' (%dx%d) format=%s dimOff=%u pixelOff=%u trailing=%u",
            path,
            width,
            height,
            outAtlas->formatTag.c_str(),
            static_cast<unsigned>(dimOffset),
            static_cast<unsigned>(pixelOffset),
            static_cast<unsigned>(trailingBytes));
        return true;
    };

    const unsigned marker = static_cast<unsigned>(bytes[0]);
    const int paletteEntryCandidatesRaw[] = {
        256,        // generated assets/replay.dat (FF + 256*3 palette bytes)
        static_cast<int>(marker), // vanilla-like files where marker indicates palette-entry count
        255,        // REPLAY.dat variant used by user (FF + 255*3 palette bytes)
        254,        // system/replay.dat variant seen in runtime logs
    };

    int paletteEntryCandidates[4] = {};
    int paletteCandidateCount = 0;
    for (const int entries : paletteEntryCandidatesRaw)
    {
        if (entries <= 0 || entries > 256)
        {
            continue;
        }
        bool duplicate = false;
        for (int i = 0; i < paletteCandidateCount; ++i)
        {
            if (paletteEntryCandidates[i] == entries)
            {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
        {
            paletteEntryCandidates[paletteCandidateCount++] = entries;
        }
    }

    for (int i = 0; i < paletteCandidateCount; ++i)
    {
        const int paletteEntries = paletteEntryCandidates[i];
        const std::size_t dimOffset = 1u + static_cast<std::size_t>(paletteEntries) * 3u;
        const std::size_t pixelOffset = dimOffset + 4u;
        char tag[64] = {};
        snprintf(tag, sizeof(tag), "palette%u", static_cast<unsigned>(paletteEntries));
        if (tryDecodeAtOffsets(paletteEntries, dimOffset, pixelOffset, tag))
        {
            return true;
        }
    }

    mod::Log(
        "ReplayHooks::LoadReplayDatAtlas: '%s' unsupported DAT layout marker=0x%02X size=%u",
        path,
        marker,
        static_cast<unsigned>(bytes.size()));
    return false;
}

bool EnsureReplayDatAtlasLoaded()
{
    if (g_replayDatAtlas.triedLoad)
    {
        return g_replayDatAtlas.loaded;
    }
    g_replayDatAtlas.triedLoad = true;

    const char* const candidates[] = {
        "REPLAY.dat",
        "replay.dat",
        "assets\\REPLAY.dat",
        "assets\\replay.dat",
        "system\\REPLAY.dat",
        "system\\replay.dat",
    };

    ReplayDatAtlas loaded = {};
    for (const char* path : candidates)
    {
        if (TryLoadReplayDatAtlasFromPath(path, &loaded))
        {
            g_replayDatAtlas = loaded;
            g_replayDatAtlas.triedLoad = true;
            g_replayDatAtlas.loaded = true;
            return true;
        }
    }

    mod::Log("ReplayHooks::LoadReplayDatAtlas: failed (no readable replay.dat found)");
    g_replayDatAtlas.loaded = false;
    return false;
}

std::uint8_t ReadReplayDatPixel(int x, int y)
{
    if (!g_replayDatAtlas.loaded || g_replayDatAtlas.width <= 0 || g_replayDatAtlas.height <= 0)
    {
        return 0;
    }
    if (x < 0 || y < 0 || x >= g_replayDatAtlas.width || y >= g_replayDatAtlas.height)
    {
        return 0;
    }
    return g_replayDatAtlas.pixelsTopDown[static_cast<std::size_t>(y) * static_cast<std::size_t>(g_replayDatAtlas.width) + static_cast<std::size_t>(x)];
}

void BuildReplayDatToRuntimePaletteLut(std::uint32_t screenContext, std::uint8_t outLut[256])
{
    if (outLut == nullptr)
    {
        return;
    }

    if (!g_replayDatAtlas.loaded || g_replayDatAtlas.paletteEntries <= 0)
    {
        for (int i = 0; i < 256; ++i)
        {
            outLut[i] = static_cast<std::uint8_t>(i);
        }
        return;
    }

    const std::uint8_t* runtimePalette = reinterpret_cast<const std::uint8_t*>(screenContext + kOffsetPalette);
    std::uint8_t lutRgb[256] = {};
    std::uint8_t lutBgr[256] = {};
    std::uint64_t scoreRgb = 0;
    std::uint64_t scoreBgr = 0;

    for (int src = 0; src < 256; ++src)
    {
        const int sr = g_replayDatAtlas.paletteRgb[src * 3 + 0];
        const int sg = g_replayDatAtlas.paletteRgb[src * 3 + 1];
        const int sb = g_replayDatAtlas.paletteRgb[src * 3 + 2];

        int bestIdxRgb = 0;
        int bestScoreRgb = 0x7FFFFFFF;
        int bestIdxBgr = 0;
        int bestScoreBgr = 0x7FFFFFFF;
        for (int dst = 0; dst < 256; ++dst)
        {
            // Interpret runtime palette as RGBx.
            const int rr0 = static_cast<int>(runtimePalette[dst * 4 + 0]);
            const int rg0 = static_cast<int>(runtimePalette[dst * 4 + 1]);
            const int rb0 = static_cast<int>(runtimePalette[dst * 4 + 2]);
            const int drRgb = rr0 - sr;
            const int dgRgb = rg0 - sg;
            const int dbRgb = rb0 - sb;
            const int score0 = drRgb * drRgb + dgRgb * dgRgb + dbRgb * dbRgb;
            if (score0 < bestScoreRgb)
            {
                bestScoreRgb = score0;
                bestIdxRgb = dst;
            }

            // Interpret runtime palette as BGRx.
            const int rr1 = static_cast<int>(runtimePalette[dst * 4 + 2]);
            const int rg1 = static_cast<int>(runtimePalette[dst * 4 + 1]);
            const int rb1 = static_cast<int>(runtimePalette[dst * 4 + 0]);
            const int drBgr = rr1 - sr;
            const int dgBgr = rg1 - sg;
            const int dbBgr = rb1 - sb;
            const int score1 = drBgr * drBgr + dgBgr * dgBgr + dbBgr * dbBgr;
            if (score1 < bestScoreBgr)
            {
                bestScoreBgr = score1;
                bestIdxBgr = dst;
            }

            if (bestScoreRgb == 0 && bestScoreBgr == 0)
            {
                break;
            }
        }

        lutRgb[src] = static_cast<std::uint8_t>(bestIdxRgb);
        lutBgr[src] = static_cast<std::uint8_t>(bestIdxBgr);
        scoreRgb += static_cast<std::uint64_t>(bestScoreRgb);
        scoreBgr += static_cast<std::uint64_t>(bestScoreBgr);
    }

    const bool useBgrRuntime = (scoreBgr < scoreRgb);
    const std::uint8_t* chosen = useBgrRuntime ? lutBgr : lutRgb;
    for (int i = 0; i < 256; ++i)
    {
        outLut[i] = chosen[i];
    }

    if (!g_replayDatPaletteMapLogged)
    {
        mod::Log(
            "ReplayHooks::ReplayDatPaletteMap: runtimeOrder=%s scoreRgb=%llu scoreBgr=%llu src[0]=%u src[20]=%u src[155]=%u src[217]=%u",
            useBgrRuntime ? "BGRx" : "RGBx",
            static_cast<unsigned long long>(scoreRgb),
            static_cast<unsigned long long>(scoreBgr),
            static_cast<unsigned>(outLut[0]),
            static_cast<unsigned>(outLut[20]),
            static_cast<unsigned>(outLut[155]),
            static_cast<unsigned>(outLut[217]));
        g_replayDatPaletteMapLogged = true;
    }
}

void BlitReplayDatRectToSurface(
    const replay::render::IndexedSurfaceView& surface,
    int dstLeftX,
    int dstTopY,
    int dstRightX,
    int dstBottomY,
    int srcLeftX,
    int srcTopY,
    int srcRightX,
    int srcBottomY,
    const std::uint8_t* paletteLut,
    int transparentRuntimeIndex)
{
    if (surface.pixels == nullptr || dstRightX < dstLeftX || dstBottomY < dstTopY)
    {
        return;
    }

    int dl = dstLeftX;
    int dt = dstTopY;
    int dr = dstRightX;
    int db = dstBottomY;
    if (dl < 0)
    {
        dl = 0;
    }
    if (dt < 0)
    {
        dt = 0;
    }
    if (dr >= surface.width)
    {
        dr = surface.width - 1;
    }
    if (db >= surface.height)
    {
        db = surface.height - 1;
    }
    if (dr < dl || db < dt)
    {
        return;
    }

    const int srcW = (srcRightX - srcLeftX + 1);
    const int srcH = (srcBottomY - srcTopY + 1);
    const int dstW = (dr - dl + 1);
    const int dstH = (db - dt + 1);
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0)
    {
        return;
    }

    for (int dy = 0; dy < dstH; ++dy)
    {
        const int sy = srcTopY + (dy * srcH) / dstH;
        std::uint8_t* outRow = surface.pixels + (dt + dy) * surface.pitch;
        for (int dx = 0; dx < dstW; ++dx)
        {
            const int sx = srcLeftX + (dx * srcW) / dstW;
            const std::uint8_t srcPx = ReadReplayDatPixel(sx, sy);
            const std::uint8_t dstPx = (paletteLut != nullptr) ? paletteLut[srcPx] : srcPx;
            if (transparentRuntimeIndex >= 0 && dstPx == static_cast<std::uint8_t>(transparentRuntimeIndex))
            {
                continue;
            }
            outRow[dl + dx] = dstPx;
        }
    }
}

void FillIndexedSurfaceRect(
    const replay::render::IndexedSurfaceView& surface,
    int leftX,
    int topY,
    int rightX,
    int bottomY,
    std::uint8_t color)
{
    if (surface.pixels == nullptr || surface.width <= 0 || surface.height <= 0)
    {
        return;
    }

    int l = leftX;
    int t = topY;
    int r = rightX;
    int b = bottomY;
    if (l < 0)
    {
        l = 0;
    }
    if (t < 0)
    {
        t = 0;
    }
    if (r >= surface.width)
    {
        r = surface.width - 1;
    }
    if (b >= surface.height)
    {
        b = surface.height - 1;
    }
    if (r < l || b < t)
    {
        return;
    }

    const int width = r - l + 1;
    for (int y = t; y <= b; ++y)
    {
        std::uint8_t* row = surface.pixels + y * surface.pitch;
        std::memset(row + l, color, static_cast<std::size_t>(width));
    }
}

bool TryLockSurfaceForWrite(void* surface, const char* label, replay::render::LockedMenuSurface* outSurface)
{
    if (surface == nullptr || outSurface == nullptr)
    {
        return false;
    }

    *outSurface = {};
    const std::uint32_t vtable = *reinterpret_cast<std::uint32_t*>(surface);
    if (vtable == 0)
    {
        return false;
    }

    const std::uint32_t lockAddress = *reinterpret_cast<std::uint32_t*>(vtable + kVtableOffsetSurfaceLock);
    if (lockAddress == 0)
    {
        return false;
    }
    auto const lockSurface =
        reinterpret_cast<HRESULT(__stdcall*)(void*, void*, std::uint32_t*, int, std::uint32_t)>(lockAddress);
    std::uint32_t lockDesc[36] = {};
    lockDesc[0] = 124;
    const HRESULT hr = lockSurface(surface, nullptr, lockDesc, 1, 0);
    if (!SUCCEEDED(hr))
    {
        return false;
    }

    const std::uintptr_t ptr9 = static_cast<std::uintptr_t>(lockDesc[9]);
    const std::uintptr_t ptr8 = static_cast<std::uintptr_t>(lockDesc[8]);
    const std::uintptr_t pixelPtr = (ptr9 != 0u) ? ptr9 : ptr8;
    if (pixelPtr == 0u || static_cast<int>(lockDesc[4]) <= 0)
    {
        const std::uint32_t unlockAddress = *reinterpret_cast<std::uint32_t*>(vtable + kVtableOffsetSurfaceUnlock);
        if (unlockAddress != 0)
        {
            auto const unlockSurface = reinterpret_cast<HRESULT(__stdcall*)(void*, void*)>(unlockAddress);
            (void)unlockSurface(surface, reinterpret_cast<void*>(pixelPtr));
        }
        return false;
    }

    outSurface->surface = surface;
    outSurface->pixels = reinterpret_cast<std::uint8_t*>(pixelPtr);
    outSurface->pitch = static_cast<int>(lockDesc[4]);
    outSurface->lockWidth = static_cast<int>(lockDesc[3]);
    outSurface->lockHeight = static_cast<int>(lockDesc[2]);
    outSurface->width = (outSurface->lockWidth > 0) ? outSurface->lockWidth : 320;
    outSurface->height = (outSurface->lockHeight > 0) ? outSurface->lockHeight : 240;
    outSurface->lockToken = reinterpret_cast<void*>(pixelPtr);
    outSurface->label = label;
    return true;
}

void ResetReplayRowLaneTrimState()
{
    g_replayRowLaneTrimState = {};
}

void CaptureReplayRowLaneSnapshot(const replay::render::LockedMenuSurface& sheetLocked)
{
    if (sheetLocked.pixels == nullptr || sheetLocked.pitch <= 0 || sheetLocked.width < 320)
    {
        return;
    }

    for (int row = 0; row < kOverlayVisibleRowsMax; ++row)
    {
        const int ySelected = kDatSelectedRowsBaseY + row * kDatRowHeight;
        const int yUnselected = kDatUnselectedRowsBaseY + row * kDatRowHeight;
        if (ySelected < 0 || (ySelected + kDatRowHeight) > sheetLocked.height
            || yUnselected < 0 || (yUnselected + kDatRowHeight) > sheetLocked.height)
        {
            return;
        }

        for (int line = 0; line < kDatRowHeight; ++line)
        {
            const std::uint8_t* srcSel = sheetLocked.pixels + (ySelected + line) * sheetLocked.pitch;
            const std::uint8_t* srcUnsel = sheetLocked.pixels + (yUnselected + line) * sheetLocked.pitch;
            std::memcpy(g_replayRowLaneTrimState.selectedRows[row] + line * 320, srcSel, 320);
            std::memcpy(g_replayRowLaneTrimState.unselectedRows[row] + line * 320, srcUnsel, 320);
        }
    }

    bool optionRowsCaptured = true;
    for (int i = 0; i < kReplayOptionRowCount; ++i)
    {
        const int yOption = kReplayOptionRowSources[i];
        if (yOption < 0 || (yOption + kDatRowHeight) > sheetLocked.height)
        {
            optionRowsCaptured = false;
            continue;
        }
        for (int line = 0; line < kDatRowHeight; ++line)
        {
            const std::uint8_t* src = sheetLocked.pixels + (yOption + line) * sheetLocked.pitch;
            std::memcpy(g_replayRowLaneTrimState.optionRows[i] + line * 320, src, 320);
        }
    }

    g_replayRowLaneTrimState.surface = sheetLocked.surface;
    g_replayRowLaneTrimState.pitch = sheetLocked.pitch;
    g_replayRowLaneTrimState.width = sheetLocked.width;
    g_replayRowLaneTrimState.height = sheetLocked.height;
    g_replayRowLaneTrimState.captured = true;
    g_replayRowLaneTrimState.optionRowsCaptured = optionRowsCaptured;
    g_replayRowLaneTrimState.lastAppliedVisibleRows = static_cast<std::size_t>(-1);
    g_replayRowLaneTrimState.lastAppliedHideOptionRows = false;
}

void ApplyReplayUiRowLaneTrim(std::uint32_t screenContext, std::size_t visibleRows)
{
    const std::size_t clampedVisible =
        (visibleRows < static_cast<std::size_t>(kOverlayVisibleRowsMax))
        ? visibleRows
        : static_cast<std::size_t>(kOverlayVisibleRowsMax);

    void* replayUiSurface = reinterpret_cast<void*>(ReadDword(screenContext, kOffsetReplayUiSurface));
    if (replayUiSurface == nullptr)
    {
        return;
    }
    const bool moving = replay::hooks::GetMenuMotionAngle() > 0.0;

    if (g_replayRowLaneTrimState.captured
        && g_replayRowLaneTrimState.surface == replayUiSurface
        && g_replayRowLaneTrimState.lastAppliedVisibleRows == clampedVisible
        && g_replayRowLaneTrimState.lastAppliedMoving == moving)
    {
        return;
    }

    replay::render::LockedMenuSurface sheetLocked = {};
    if (!TryLockSurfaceForWrite(replayUiSurface, "replay-sheet", &sheetLocked))
    {
        return;
    }

    const bool snapshotMismatch =
        !g_replayRowLaneTrimState.captured
        || g_replayRowLaneTrimState.surface != replayUiSurface
        || g_replayRowLaneTrimState.pitch != sheetLocked.pitch
        || g_replayRowLaneTrimState.width != sheetLocked.width
        || g_replayRowLaneTrimState.height != sheetLocked.height;
    if (snapshotMismatch)
    {
        CaptureReplayRowLaneSnapshot(sheetLocked);
    }

    if (sheetLocked.width >= 320)
    {
        // The colour key vanilla passes when blitting this sheet: cleared pixels vanish.
        const std::uint8_t clearColor = ReadByte(screenContext, kOffsetReplayTransparentColor);
        std::uint8_t paletteLut[256] = {};
        const bool hasReplayDat = EnsureReplayDatAtlasLoaded();
        if (hasReplayDat)
        {
            BuildReplayDatToRuntimePaletteLut(screenContext, paletteLut);
        }

        for (int row = 0; row < kOverlayVisibleRowsMax; ++row)
        {
            const int ySelected = kDatSelectedRowsBaseY + row * kDatRowHeight;
            const int yUnselected = kDatUnselectedRowsBaseY + row * kDatRowHeight;
            if (ySelected < 0 || (ySelected + kDatRowHeight) > sheetLocked.height
                || yUnselected < 0 || (yUnselected + kDatRowHeight) > sheetLocked.height)
            {
                continue;
            }

            for (int line = 0; line < kDatRowHeight; ++line)
            {
                std::uint8_t* dstSel = sheetLocked.pixels + (ySelected + line) * sheetLocked.pitch;
                std::uint8_t* dstUnsel = sheetLocked.pixels + (yUnselected + line) * sheetLocked.pitch;
                // Lines 12-15 are the gap under a bar; the mod's REPLAY.dat draws the next
                // bar's top edge on line 15, which must not show under the last visible row,
                // nor under any row while the motion pulls them apart.
                const bool hideGap = (moving || static_cast<std::size_t>(row) + 1 == clampedVisible) && line >= 12;
                if (static_cast<std::size_t>(row) < clampedVisible && !hideGap)
                {
                    if (hasReplayDat)
                    {
                        std::uint8_t rowSel[320] = {};
                        std::uint8_t rowUnsel[320] = {};
                        const int srcYSelected = ySelected + line;
                        const int srcYUnselected = yUnselected + line;
                        for (int x = 0; x < 320; ++x)
                        {
                            rowSel[x] = paletteLut[ReadReplayDatPixel(x, srcYSelected)];
                            rowUnsel[x] = paletteLut[ReadReplayDatPixel(x, srcYUnselected)];
                        }
                        std::memcpy(dstSel, rowSel, 320);
                        std::memcpy(dstUnsel, rowUnsel, 320);
                    }
                    else if (g_replayRowLaneTrimState.captured)
                    {
                        const std::uint8_t* srcSel = g_replayRowLaneTrimState.selectedRows[row] + line * 320;
                        const std::uint8_t* srcUnsel = g_replayRowLaneTrimState.unselectedRows[row] + line * 320;
                        std::memcpy(dstSel, srcSel, 320);
                        std::memcpy(dstUnsel, srcUnsel, 320);
                    }
                    else
                    {
                        std::memset(dstSel, clearColor, 320);
                        std::memset(dstUnsel, clearColor, 320);
                    }
                }
                else
                {
                    std::memset(dstSel, clearColor, 320);
                    std::memset(dstUnsel, clearColor, 320);
                }
            }
        }

        // Option rows (PLAY REPLAY / RECORD REPLAY bars) are always blanked so the vanilla
        // renderer can never blit them regardless of menuState or D-menu state.
        for (int i = 0; i < kReplayOptionRowCount; ++i)
        {
            const int yOption = kReplayOptionRowSources[i];
            if (yOption < 0 || (yOption + kDatRowHeight) > sheetLocked.height)
            {
                continue;
            }
            for (int line = 0; line < kDatRowHeight; ++line)
            {
                std::uint8_t* dst = sheetLocked.pixels + (yOption + line) * sheetLocked.pitch;
                std::memset(dst, clearColor, 320);
            }
        }

        g_replayRowLaneTrimState.lastAppliedVisibleRows = clampedVisible;
        g_replayRowLaneTrimState.lastAppliedMoving = moving;
        g_replayRowLaneTrimState.lastAppliedHideOptionRows = true;
        mod::Log(
            "ReplayHooks::ApplyReplayUiRowLaneTrim: visibleRows=%u hiddenRows=%u src=%s surface=0x%08X",
            static_cast<unsigned>(clampedVisible),
            static_cast<unsigned>(static_cast<std::size_t>(kOverlayVisibleRowsMax) - clampedVisible),
            hasReplayDat ? "replay.dat" : "snapshot",
            static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(replayUiSurface)));
    }

    replay::render::ReleaseMenuDrawSurfaceLock(sheetLocked);
}

void DrawMappedReplayBars(
    std::uint32_t screenContext,
    const replay::render::IndexedSurfaceView& surface,
    int leftX,
    int rightX,
    int rowHeightPx,
    int yNumerator,
    int yDenominator,
    std::size_t rowCount,
    std::size_t selectedRow)
{
    if (surface.pixels == nullptr || rowHeightPx <= 0 || rightX < leftX || yNumerator <= 0 || yDenominator <= 0)
    {
        return;
    }
    if (!EnsureReplayDatAtlasLoaded())
    {
        return;
    }
    std::uint8_t paletteLut[256] = {};
    BuildReplayDatToRuntimePaletteLut(screenContext, paletteLut);
    const int transparentRuntimeIndex = static_cast<int>(ReadByte(screenContext, kOffsetReplayTransparentColor));

    const std::size_t rowsToDraw =
        (rowCount < static_cast<std::size_t>(kOverlayVisibleRowsMax))
        ? rowCount
        : static_cast<std::size_t>(kOverlayVisibleRowsMax);
    if (rowsToDraw == 0)
    {
        return;
    }

    const std::size_t clampedSelected = (selectedRow < rowsToDraw) ? selectedRow : 0;
    for (std::size_t row = 0; row < rowsToDraw; ++row)
    {
        const int rowIndex = static_cast<int>(row);
        const int baseYNative = kOverlayBaseYNative + rowIndex * kOverlayRowStepNative;
        const bool isSelected = (row == clampedSelected);
        const int drawYNative = baseYNative;
        const int dstTopY = MulDiv(drawYNative, yNumerator, yDenominator);
        const int dstBottomY = dstTopY + rowHeightPx - 1;
        const int base = isSelected ? kDatSelectedRowsBaseY : kDatUnselectedRowsBaseY;
        const int srcTopY = base + rowIndex * kDatRowHeight;
        const int srcBottomY = srcTopY + kDatRowHeight - 1;

        BlitReplayDatRectToSurface(
            surface,
            leftX,
            dstTopY,
            rightX,
            dstBottomY,
            kDatLaneX0,
            srcTopY,
            kDatLaneX1,
            srcBottomY,
            paletteLut,
            transparentRuntimeIndex);
    }
}

char LaunchReplayNow(std::uint32_t screenContext, const char* reason)
{
    if (g_fadeWithSoundAdjustment != nullptr)
    {
        // Blocking fade-out to battle: it re-enters render 21x while dimming the palette.
        (void)g_fadeWithSoundAdjustment(reinterpret_cast<void*>(screenContext), static_cast<int>(screenContext + 46), 0, 0, 50);
    }
    else
    {
        mod::Log("ReplayHooks::LaunchReplayNow: fade helper missing, launching replay without fade");
    }

    WriteByte(screenContext, kOffsetFirstTimeFlag, 2);
    WriteByte(screenContext, kOffsetMenuState, 0);
    mod::Log(
        "ReplayHooks::LaunchReplayNow: reason='%s' -> return 2",
        (reason != nullptr) ? reason : "(null)");
    return 2;
}

std::string LowerAscii(std::string value)
{
    for (char& c : value)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return value;
}

bool IsVanillaReplayPath(const char* filePath)
{
    if (filePath == nullptr || *filePath == '\0')
    {
        return false;
    }
    const std::string lower = LowerAscii(filePath);
    const bool startsLikeReplayData = lower.rfind("replay\\data", 0) == 0 || lower.rfind("replay/data", 0) == 0;
    const bool endsRep = lower.size() >= 4 && lower.substr(lower.size() - 4) == ".rep";
    return startsLikeReplayData && endsRep;
}

// ReplayOpener-style path swap: while the game's openFileReader runs for the vanilla
// replay/dataX-Y.rep, CreateFileA/W for exactly that name open the selected file via
// its UTF-16 path instead. Scoped to that call and thread so unrelated opens pass
// through, and active for the whole call: Revival opens a probe stream, creates the
// file if the probe fails, then opens the real stream.
using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
CreateFileWFn g_originalCreateFileW = nullptr;
CreateFileAFn g_originalCreateFileA = nullptr;
bool g_createFileHooksAttempted = false;
bool g_createFileHooksInstalled = false;
void* g_createFileWTarget = nullptr;
void* g_createFileATarget = nullptr;
thread_local const std::wstring* t_redirectFrom = nullptr; // vanilla relative name, '\' separators
thread_local const std::wstring* t_redirectTo = nullptr;

bool MatchesRedirect(std::wstring path)
{
    if (t_redirectFrom == nullptr || t_redirectTo == nullptr)
    {
        return false;
    }
    std::replace(path.begin(), path.end(), L'/', L'\\');
    const std::wstring& from = *t_redirectFrom;
    if (path.size() < from.size()
        || _wcsicmp(path.c_str() + (path.size() - from.size()), from.c_str()) != 0)
    {
        return false;
    }
    return path.size() == from.size() || path[path.size() - from.size() - 1] == L'\\';
}

// Revival creates the vanilla file ("wb") when its read probe fails. Refuse it for
// the redirected name: redirecting would truncate the user's replay, passing it
// through would wipe the real vanilla slot file.
bool IsTruncatingOpen(DWORD disposition)
{
    return disposition == CREATE_ALWAYS || disposition == TRUNCATE_EXISTING;
}

HANDLE WINAPI HookedCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
    DWORD disposition, DWORD flags, HANDLE templateFile)
{
    if (name != nullptr && t_redirectFrom != nullptr && MatchesRedirect(name))
    {
        if (IsTruncatingOpen(disposition))
        {
            SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        name = t_redirectTo->c_str();
    }
    return g_originalCreateFileW(name, access, share, sa, disposition, flags, templateFile);
}

HANDLE WINAPI HookedCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
    DWORD disposition, DWORD flags, HANDLE templateFile)
{
    if (name != nullptr && t_redirectFrom != nullptr && MatchesRedirect(std::wstring(name, name + std::strlen(name))))
    {
        if (IsTruncatingOpen(disposition))
        {
            SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        return g_originalCreateFileW(t_redirectTo->c_str(), access, share, sa, disposition, flags, templateFile);
    }
    return g_originalCreateFileA(name, access, share, sa, disposition, flags, templateFile);
}

bool EnsureCreateFileHooksInstalled()
{
    if (g_createFileHooksAttempted)
    {
        return g_createFileHooksInstalled;
    }
    g_createFileHooksAttempted = true;
    // KernelBase holds the real bodies; kernel32's exports forward into them, so both
    // kernel32 importers and api-set (UCRT) callers are covered.
    HMODULE kernelBase = GetModuleHandleA("kernelbase.dll");
    if (kernelBase == nullptr)
    {
        kernelBase = GetModuleHandleA("kernel32.dll");
    }
    void* createFileW = reinterpret_cast<void*>(GetProcAddress(kernelBase, "CreateFileW"));
    void* createFileA = reinterpret_cast<void*>(GetProcAddress(kernelBase, "CreateFileA"));
    g_createFileWTarget = createFileW;
    g_createFileATarget = createFileA;
    if (createFileW == nullptr || createFileA == nullptr
        || !replay::hook::Install(createFileW, reinterpret_cast<void*>(&HookedCreateFileW),
            reinterpret_cast<void**>(&g_originalCreateFileW), "CreateFileW"))
    {
        return false;
    }
    if (!replay::hook::Install(createFileA, reinterpret_cast<void*>(&HookedCreateFileA),
            reinterpret_cast<void**>(&g_originalCreateFileA), "CreateFileA"))
    {
        (void)MH_DisableHook(createFileW);
        return false;
    }
    g_createFileHooksInstalled = true;
    mod::Log("ReplayHooks::EnsureCreateFileHooksInstalled: installed W=%p A=%p", createFileW, createFileA);
    return true;
}

std::string PathForLog(const std::filesystem::path& path)
{
    const auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
}

std::filesystem::path GetAsciiReplayAliasPath()
{
    return std::filesystem::path("replay") / "__efz_open_alias.rep";
}

void CleanupAsciiReplayAlias()
{
    std::error_code ec = {};
    const std::filesystem::path aliasPath = GetAsciiReplayAliasPath();
    (void)std::filesystem::remove(aliasPath, ec);
    if (ec)
    {
        mod::Log(
            "ReplayHooks::CleanupAsciiReplayAlias: remove failed alias='%s' ec=%d",
            PathForLog(aliasPath).c_str(),
            ec.value());
    }
}

// Draws the replay overlay into each presented frame. Installed lazily on the game
// thread (first replay-screen frame), never during process startup.
bool EnsurePresentHookInstalled()
{
    if (g_presentFrameToScreenTarget != nullptr)
    {
        return true;
    }
    void* target = reinterpret_cast<void*>(RuntimeAddress(kVaPresentFrameToScreen));
    if (!replay::hook::Install(target, reinterpret_cast<void*>(&HookedPresentFrameToScreenThunk),
            reinterpret_cast<void**>(&g_originalPresentFrameToScreen), "presentFrameToScreen"))
    {
        g_originalPresentFrameToScreen = nullptr;
        return false;
    }
    g_presentFrameToScreenTarget = target;
    return true;
}

bool EnsureReplayOpenOverrideHookInstalled()
{
    if (g_originalOpenFileReader != nullptr)
    {
        return true;
    }

    // MinHook relocates an existing JMP (Revival hooks this entry), so the chain is kept.
    void* target = reinterpret_cast<void*>(RuntimeAddress(kVaOpenFileReader));
    if (!replay::hook::Install(target, reinterpret_cast<void*>(&HookedOpenFileReaderThunk),
            reinterpret_cast<void**>(&g_originalOpenFileReader), "openFileReader"))
    {
        g_originalOpenFileReader = nullptr;
        return false;
    }
    g_openFileReaderTarget = target;
    return true;
}

bool PatchCString(
    std::uintptr_t va,
    const char* expectedText,
    const char* replacementText,
    const char* label)
{
    if (expectedText == nullptr || replacementText == nullptr)
    {
        mod::Log("ReplayHooks::PatchCString: invalid args label='%s'", (label != nullptr) ? label : "(null)");
        return false;
    }

    std::string expected(expectedText);
    std::string replacement(replacementText);
    if (expected.size() != replacement.size())
    {
        mod::Log(
            "ReplayHooks::PatchCString: length mismatch label='%s' expectedLen=%u replacementLen=%u",
            (label != nullptr) ? label : "(null)",
            static_cast<unsigned>(expected.size()),
            static_cast<unsigned>(replacement.size()));
        return false;
    }

    std::vector<std::uint8_t> expectedBytes(expected.begin(), expected.end());
    expectedBytes.push_back(0);
    std::vector<std::uint8_t> patchedBytes(replacement.begin(), replacement.end());
    patchedBytes.push_back(0);

    return replay::patch::ApplyPatch(
        RuntimeAddress(va),
        expectedBytes,
        patchedBytes,
        label,
        &g_appliedPatches);
}

bool FileExistsA(const char* path)
{
    if (path == nullptr || *path == '\0')
    {
        return false;
    }

    const DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES)
    {
        return false;
    }
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool PatchReplayAssetPathIfPresent(
    std::uintptr_t va,
    const char* expectedText,
    const char* replacementText,
    const char* label)
{
    if (!FileExistsA(replacementText))
    {
        mod::Log(
            "ReplayHooks::PatchReplayAssetPathIfPresent: replacement missing label='%s' path='%s' -> keep vanilla='%s'",
            (label != nullptr) ? label : "(null)",
            (replacementText != nullptr) ? replacementText : "(null)",
            (expectedText != nullptr) ? expectedText : "(null)");
        return true;
    }

    mod::Log(
        "ReplayHooks::PatchReplayAssetPathIfPresent: replacement found label='%s' path='%s' -> patching",
        (label != nullptr) ? label : "(null)",
        replacementText);
    return PatchCString(va, expectedText, replacementText, label);
}

std::string BuildReplayOverlayText(const replay::browser::ReplayBrowserEntry& entry)
{
    if (entry.kind == replay::browser::BrowserEntryKind::Directory)
    {
        return std::string("[") + entry.displayName + "]";
    }
    return entry.displayName;
}

bool ContainsNonAsciiText(const std::string& text)
{
    for (unsigned char c : text)
    {
        if ((c & 0x80u) != 0)
        {
            return true;
        }
    }
    return false;
}

bool DecodeUtf8Codepoint(const std::string& text, std::size_t* index, unsigned int* outCodepoint)
{
    if (index == nullptr || outCodepoint == nullptr || *index >= text.size())
    {
        return false;
    }

    const std::size_t i = *index;
    const unsigned char b0 = static_cast<unsigned char>(text[i]);
    if ((b0 & 0x80u) == 0)
    {
        *outCodepoint = static_cast<unsigned int>(b0);
        *index = i + 1;
        return true;
    }

    if ((b0 & 0xE0u) == 0xC0u && i + 1 < text.size())
    {
        const unsigned char b1 = static_cast<unsigned char>(text[i + 1]);
        if ((b1 & 0xC0u) == 0x80u)
        {
            *outCodepoint = static_cast<unsigned int>(((b0 & 0x1Fu) << 6) | (b1 & 0x3Fu));
            *index = i + 2;
            return true;
        }
    }
    else if ((b0 & 0xF0u) == 0xE0u && i + 2 < text.size())
    {
        const unsigned char b1 = static_cast<unsigned char>(text[i + 1]);
        const unsigned char b2 = static_cast<unsigned char>(text[i + 2]);
        if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u)
        {
            *outCodepoint = static_cast<unsigned int>(((b0 & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) | (b2 & 0x3Fu));
            *index = i + 3;
            return true;
        }
    }
    else if ((b0 & 0xF8u) == 0xF0u && i + 3 < text.size())
    {
        const unsigned char b1 = static_cast<unsigned char>(text[i + 1]);
        const unsigned char b2 = static_cast<unsigned char>(text[i + 2]);
        const unsigned char b3 = static_cast<unsigned char>(text[i + 3]);
        if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u && (b3 & 0xC0u) == 0x80u)
        {
            *outCodepoint = static_cast<unsigned int>(((b0 & 0x07u) << 18) | ((b1 & 0x3Fu) << 12)
                | ((b2 & 0x3Fu) << 6) | (b3 & 0x3Fu));
            *index = i + 4;
            return true;
        }
    }

    *outCodepoint = static_cast<unsigned int>(b0);
    *index = i + 1;
    return false;
}

bool ContainsJapaneseCodepointUtf8(const std::string& text)
{
    std::size_t i = 0;
    while (i < text.size())
    {
        unsigned int cp = 0;
        (void)DecodeUtf8Codepoint(text, &i, &cp);
        if ((cp >= 0x3040u && cp <= 0x30FFu)
            || (cp >= 0x31F0u && cp <= 0x31FFu)
            || (cp >= 0xFF66u && cp <= 0xFF9Fu)
            || (cp >= 0x4E00u && cp <= 0x9FFFu))
        {
            return true;
        }
    }
    return false;
}

// Current screen fade, read from the DirectDraw palette the frame is presented with:
// createDarkenedPalette scales the base palette (screen+46) by p/100,
// createLightenedPalette moves it toward white by (100-p)/100.
struct PaletteFade
{
    float darken = 1.0f;
    float lighten = 0.0f;
};
PaletteFade g_frameFade;

PaletteFade ReadPaletteFade(std::uint32_t screenContext)
{
    PaletteFade fade;
    const std::uint32_t graphicsContext = ReadDword(screenContext, kOffsetGraphicsContext);
    void* palette = (graphicsContext != 0)
        ? reinterpret_cast<void*>(ReadDword(graphicsContext, kGraphicsPaletteOffset)) : nullptr;
    if (palette == nullptr)
    {
        return fade;
    }
    using GetEntriesFn = HRESULT(__stdcall*)(void*, DWORD, DWORD, DWORD, std::uint8_t*);
    const auto getEntries = reinterpret_cast<GetEntriesFn>((*reinterpret_cast<void***>(palette))[4]);
    std::uint8_t current[256 * 4] = {};
    if (FAILED(getEntries(palette, 0, 0, 255, current)))
    {
        return fade;
    }
    const auto* base = reinterpret_cast<const std::uint8_t*>(screenContext + kOffsetPalette);
    long baseSum = 0;
    long currentSum = 0;
    long headroom = 0;
    for (int i = 0; i < 255; ++i)
    {
        for (int c = 0; c < 3; ++c)
        {
            baseSum += base[4 * i + c];
            currentSum += current[4 * i + c];
            headroom += 255 - base[4 * i + c];
        }
    }
    if (currentSum < baseSum && baseSum > 0)
    {
        fade.darken = static_cast<float>(currentSum) / static_cast<float>(baseSum);
    }
    else if (currentSum > baseSum && headroom > 0)
    {
        fade.lighten = static_cast<float>(currentSum - baseSum) / static_cast<float>(headroom);
    }
    if (fade.darken > 0.98f)
    {
        fade.darken = 1.0f;
    }
    if (fade.lighten < 0.02f)
    {
        fade.lighten = 0.0f;
    }
    return fade;
}

std::uint32_t ApplyFade(std::uint32_t argb)
{
    std::uint32_t out = argb & 0xFF000000u;
    for (int shift = 0; shift <= 16; shift += 8)
    {
        float c = static_cast<float>((argb >> shift) & 0xFFu) * g_frameFade.darken;
        c += (255.0f - c) * g_frameFade.lighten;
        out |= static_cast<std::uint32_t>(c + 0.5f) << shift;
    }
    return out;
}

void SubmitFaded(replay::render::d3d9::TextItem item)
{
    item.argb = ApplyFade(item.argb);
    if (item.fillArgb != 0)
    {
        item.fillArgb = ApplyFade(item.fillArgb);
    }
    replay::render::d3d9::Submit(item);
}

double g_optionsHighlightY = -1.0;

int AdvanceOptionsHighlight(std::size_t selected)
{
    const double target = kOptionsItemStartYNative + static_cast<double>(selected) * kOptionsItemStepNative;
    const double distance = target - g_optionsHighlightY;
    if (g_optionsHighlightY < 0.0 || std::fabs(distance) < 0.75 || std::fabs(distance) > 96.0)
    {
        g_optionsHighlightY = target;
    }
    else
    {
        g_optionsHighlightY += distance * 0.34;
    }
    return static_cast<int>(g_optionsHighlightY + 0.5);
}

void DrawOptionsPanel(
    const replay::render::IndexedSurfaceView& surface,
    int xDenom,
    int yDenom,
    int fontScaleX,
    int fontScaleY,
    std::uint8_t colorSelected,
    std::uint8_t colorNormal,
    const replay::hooks::ReplayOverlayMenuState& menuState,
    bool useRt)
{
    using replay::i18n::Text;
    using replay::i18n::Tr;
    if (!menuState.open)
    {
        g_optionsHighlightY = -1.0;
        return;
    }

    const int panelOffset = replay::motion::PanelOffset(replay::hooks::GetMenuMotionAngle());
    const auto px = [&](int native) { return MulDiv(native, surface.width, xDenom); };
    const auto py = [&](int native) { return MulDiv(native + panelOffset, surface.height, yDenom); };
    const int left = px(kOptionsPanelLeftNative);
    const int right = px(kOptionsPanelRightNative);
    const int top = py(kOptionsPanelTopNative);
    const int bottom = py(kOptionsPanelBottomNative);

    FillIndexedSurfaceRect(surface, left, top, right, bottom, kOverlayTextClearColorIndex);
    FillIndexedSurfaceRect(surface, left, top, right, top, colorNormal);
    FillIndexedSurfaceRect(surface, left, bottom, right, bottom, colorNormal);
    FillIndexedSurfaceRect(surface, left, top, left, bottom, colorNormal);
    FillIndexedSurfaceRect(surface, right, top, right, bottom, colorNormal);
    const int titleSeparator = py(kOptionsPanelTopNative + kOptionsTitleHeightNative);
    FillIndexedSurfaceRect(surface, left, titleSeparator, right, titleSeparator, colorNormal);

    const int tipLeft = px(kOptionsTipLeftNative);
    const int tipRight = px(kOptionsTipRightNative);
    const int tipTop = py(kOptionsTipTopNative);
    const int tipBottom = py(kOptionsTipBottomNative);
    FillIndexedSurfaceRect(surface, tipLeft, tipTop, tipRight, tipBottom, kOverlayTextClearColorIndex);
    FillIndexedSurfaceRect(surface, tipLeft, tipTop, tipRight, tipTop, colorNormal);
    FillIndexedSurfaceRect(surface, tipLeft, tipBottom, tipRight, tipBottom, colorNormal);
    FillIndexedSurfaceRect(surface, tipLeft, tipTop, tipLeft, tipBottom, colorNormal);
    FillIndexedSurfaceRect(surface, tipRight, tipTop, tipRight, tipBottom, colorNormal);

    const std::size_t selected = (menuState.selectedIndex < kOptionsItemCount) ? menuState.selectedIndex : 0u;
    const int highlightTop = AdvanceOptionsHighlight(selected);
    FillIndexedSurfaceRect(surface, left + px(2), py(highlightTop), right - px(2),
        py(highlightTop + kOptionsHighlightHeightNative) - 1, colorNormal);

    const auto submit = [panelOffset](const char* text, int topNative, int heightNative, int x0, int x1,
                            replay::render::d3d9::TextAlign align, std::uint32_t argb) {
        replay::render::d3d9::TextItem item = {};
        item.x0 = static_cast<std::int16_t>(x0);
        item.x1 = static_cast<std::int16_t>(x1);
        item.y = static_cast<std::int16_t>(topNative + panelOffset);
        item.h = static_cast<std::int16_t>(heightNative);
        item.align = align;
        item.argb = argb;
        item.smallText = true;
        std::snprintf(item.text, sizeof(item.text), "%s", text);
        SubmitFaded(item);
    };
    const auto software = [&](const std::string& text, int topNative, int heightNative, std::uint8_t color) {
        const int glyphHeight = 7 * fontScaleY;
        const int y = py(topNative) + std::max(0, (py(topNative + heightNative) - py(topNative) - glyphHeight) / 2);
        replay::render::DrawTextCentered5x7(surface, text, left + px(kOptionsTextPadNative),
            right - px(kOptionsTextPadNative), y, fontScaleX, fontScaleY, color);
    };
    using Align = replay::render::d3d9::TextAlign;
    const int textLeft = kOptionsPanelLeftNative + kOptionsTextPadNative;
    const int textRight = kOptionsPanelRightNative - kOptionsTextPadNative;

    const int aboutSeparator = py(kOptionsAboutYNative - 2);
    FillIndexedSurfaceRect(surface, left + px(4), aboutSeparator, right - px(4), aboutSeparator, colorNormal);
    char aboutName[96] = {};
    std::snprintf(aboutName, sizeof(aboutName), "%s v%s", replay::version::kName, replay::version::kVersion);
    char aboutBuild[96] = {};
    std::snprintf(aboutBuild, sizeof(aboutBuild), "%s %s", Tr(Text::Build), replay::version::BuildTime());

    char filterCount[96] = {};
    std::snprintf(filterCount, sizeof(filterCount), Tr(Text::FilterCountFmt), menuState.shownReplays, menuState.totalReplays);
    const char* const sortValue = (menuState.sortOrder == replay::browser::ReplaySortOrder::NewestFirst) ? Tr(Text::SortNewest)
        : (menuState.sortOrder == replay::browser::ReplaySortOrder::OldestFirst)                         ? Tr(Text::SortOldest)
        : (menuState.sortOrder == replay::browser::ReplaySortOrder::PlayerName)                          ? Tr(Text::SortPlayer)
                                                                                                         : Tr(Text::SortLargest);
    const char* const title = menuState.keysPage ? Tr(Text::RewindKeys)
        : menuState.filterPage                   ? Tr(Text::FilterTitle)
                                                 : Tr(Text::OptionsTitle);
    static constexpr Text kOptionsTips[] = {Text::TipRecord, Text::TipSort, Text::TipPlayFolder, Text::TipRefresh,
        Text::TipBackground, Text::TipLanguage, Text::TipFrameCounter, Text::TipRewindKeys};
    static constexpr Text kKeysTips[] = {Text::TipKeyBack, Text::TipKeyForward, Text::TipKeyPrevRound,
        Text::TipKeyNextRound, Text::TipKeyToggleCounter, Text::TipKeysRestore};
    static constexpr Text kFilterTips[] = {Text::TipFilterSort, Text::TipFilterCharacter, Text::TipFilterCharacter2,
        Text::TipFilterPlayer, Text::TipFilterClear};
    static_assert(std::size(kOptionsTips) == kOptionsItemCount);
    static_assert(std::size(kKeysTips) == replay::hooks::kRewindKeysPageRows);
    static_assert(std::size(kFilterTips) == static_cast<std::size_t>(replay::hooks::FilterPageRow::Count));
    const std::size_t tipRow = menuState.selectedIndex;
    const char* const tip = menuState.keysPage ? Tr(kKeysTips[tipRow < std::size(kKeysTips) ? tipRow : 0])
        : menuState.filterPage ? Tr(kFilterTips[tipRow < std::size(kFilterTips) ? tipRow : 0])
                               : Tr(kOptionsTips[tipRow < std::size(kOptionsTips) ? tipRow : 0]);
    const auto optionsRow = static_cast<replay::hooks::OptionsItem>(tipRow);
    const char* const hint = menuState.filterPage
        ? Tr(tipRow == static_cast<std::size_t>(replay::hooks::FilterPageRow::Clear) ? Text::HintFilterClear : Text::FilterHint)
        : !menuState.keysPage
        ? Tr(optionsRow == replay::hooks::OptionsItem::Sort ? Text::HintOptionsSort
                : (optionsRow == replay::hooks::OptionsItem::BackgroundScroll || optionsRow == replay::hooks::OptionsItem::Language
                    || optionsRow == replay::hooks::OptionsItem::FrameCounter)
                ? Text::HintOptionsValue
                : Text::HintOptionsAction)
        : (menuState.captureRow < 0) ? Tr(Text::KeysHint)
        : menuState.capturePrimed    ? Tr(Text::KeysPress)
                                     : Tr(Text::KeysRelease);
    const int tipTextLeft = kOptionsTipLeftNative + 4;
    const int tipTextRight = kOptionsTipRightNative - 4;
    const char* const lane0 = menuState.keysPage ? "" : menuState.filterPage ? filterCount : Tr(Text::About);
    const char* const lane1 = menuState.keysPage ? menuState.notice : menuState.filterPage ? "" : aboutName;
    const char* const lane2 = (menuState.keysPage || menuState.filterPage) ? "" : aboutBuild;

    if (useRt)
    {
        submit(lane0, kOptionsAboutYNative, kOptionsAboutLineNative, textLeft, textRight, Align::Center,
            menuState.keysPage ? kRtTextNormalArgb : kRtTextSelectedArgb);
        submit(lane1, kOptionsAboutYNative + kOptionsAboutLineNative, kOptionsAboutLineNative, textLeft, textRight,
            Align::Center, menuState.keysPage ? kRtTextSelectedArgb : kRtTextNormalArgb);
        submit(lane2, kOptionsAboutYNative + 2 * kOptionsAboutLineNative, kOptionsAboutLineNative, textLeft,
            textRight, Align::Center, kRtTextHintArgb);
        submit(title, kOptionsPanelTopNative + 1, kOptionsTitleHeightNative - 1, textLeft, textRight,
            Align::Center, kRtTextSelectedArgb);
        submit(tip, kOptionsTipTopNative + 2, kOptionsTipLineNative, tipTextLeft, tipTextRight, Align::Center,
            kRtTextSelectedArgb);
        submit(hint, kOptionsTipTopNative + 2 + kOptionsTipLineNative, kOptionsTipLineNative, tipTextLeft, tipTextRight,
            Align::Center, kRtTextHintArgb);
    }
    else
    {
        software(title, kOptionsPanelTopNative + 1, kOptionsTitleHeightNative - 1, colorSelected);
        software(lane0, kOptionsAboutYNative, kOptionsAboutLineNative, menuState.keysPage ? colorNormal : colorSelected);
        software(lane1, kOptionsAboutYNative + kOptionsAboutLineNative, kOptionsAboutLineNative, menuState.keysPage ? colorSelected : colorNormal);
        software(lane2, kOptionsAboutYNative + 2 * kOptionsAboutLineNative, kOptionsAboutLineNative, colorNormal);
        const auto softwareTip = [&](const char* text, int topNative) {
            const int y = py(topNative) + std::max(0, (py(topNative + kOptionsTipLineNative) - py(topNative) - 7 * fontScaleY) / 2);
            replay::render::DrawTextCentered5x7(surface, text, tipLeft + px(4), tipRight - px(4), y, fontScaleX, fontScaleY, colorNormal);
        };
        softwareTip(tip, kOptionsTipTopNative + 2);
        softwareTip(hint, kOptionsTipTopNative + 2 + kOptionsTipLineNative);
    }

    const replay::settings::Language language = replay::settings::GetLanguage();
    const char* languageValue = (language == replay::settings::Language::Japanese) ? "日本語"
        : (language == replay::settings::Language::English)                        ? "English"
                                                                                   : Tr(Text::LanguageAuto);
    const std::size_t rowCount = menuState.keysPage ? replay::hooks::kRewindKeysPageRows
        : menuState.filterPage ? static_cast<std::size_t>(replay::hooks::FilterPageRow::Count)
                               : kOptionsItemCount - (menuState.rewindAvailable ? 0u : 1u);
    for (std::size_t i = 0; i < rowCount; ++i)
    {
        const auto itemId = static_cast<replay::hooks::OptionsItem>(i);
        const char* label = "";
        const char* value = nullptr;
        std::string keyValue;
        if (menuState.keysPage)
        {
            static constexpr Text kKeyLabels[] = {
                Text::KeyBack, Text::KeyForward, Text::KeyPrevRound, Text::KeyNextRound, Text::KeyToggleCounter, Text::KeysRestore};
            label = Tr(kKeyLabels[i]);
            if (i + 1u < rowCount)
            {
                keyValue = (static_cast<int>(i) == menuState.captureRow)
                    ? std::string("...")
                    : replay::i18n::KeyName(replay::settings::GetRewindKey(static_cast<replay::settings::RewindKey>(i)));
                value = keyValue.c_str();
            }
        }
        else if (menuState.filterPage)
        {
            switch (static_cast<replay::hooks::FilterPageRow>(i))
            {
            case replay::hooks::FilterPageRow::Sort:
                label = Tr(Text::Sort);
                value = sortValue;
                break;
            case replay::hooks::FilterPageRow::Character1:
            case replay::hooks::FilterPageRow::Character2:
            {
                const bool first = static_cast<replay::hooks::FilterPageRow>(i) == replay::hooks::FilterPageRow::Character1;
                label = Tr(first ? Text::FilterCharacter : Text::FilterCharacter2);
                const char* const character = replay::name::ReplayCharacterName(menuState.filterCharacters[first ? 0 : 1]);
                value = (character != nullptr) ? character : Tr(Text::FilterAll);
                break;
            }
            case replay::hooks::FilterPageRow::Player:
                label = Tr(Text::FilterPlayer);
                value = (menuState.filterPlayer[0] != '\0') ? menuState.filterPlayer : Tr(Text::FilterAll);
                break;
            default:
                label = Tr(Text::FilterClear);
                break;
            }
        }
        else switch (itemId)
        {
        case replay::hooks::OptionsItem::RewindKeys: label = Tr(Text::RewindKeys); break;
        case replay::hooks::OptionsItem::RecordReplay: label = Tr(Text::RecordReplay); break;
        case replay::hooks::OptionsItem::Sort:
            label = Tr(Text::SortFilterRow);
            value = sortValue;
            break;
        case replay::hooks::OptionsItem::PlayFolder: label = Tr(Text::PlayFolder); break;
        case replay::hooks::OptionsItem::RefreshFolder: label = Tr(Text::RefreshFolder); break;
        case replay::hooks::OptionsItem::BackgroundScroll:
            label = Tr(Text::BackgroundScroll);
            value = Tr(replay::settings::GetBackgroundScroll() ? Text::On : Text::Off);
            break;
        case replay::hooks::OptionsItem::FrameCounter:
            label = Tr(Text::FrameCounter);
            value = Tr(replay::settings::GetFrameCounter() ? Text::On : Text::Off);
            break;
        case replay::hooks::OptionsItem::Language:
            label = Tr(Text::Language);
            value = languageValue;
            break;
        default: break;
        }
        const int rowTop = kOptionsItemStartYNative + static_cast<int>(i) * kOptionsItemStepNative;
        const bool isSelected = (i == selected);
        if (useRt)
        {
            const std::uint32_t argb = isSelected ? kRtTextOnHighlightArgb : kRtTextNormalArgb;
            if (menuState.filterPage && static_cast<replay::hooks::FilterPageRow>(i) == replay::hooks::FilterPageRow::Player)
            {
                constexpr int kPlayerLabelNative = 40;
                submit(label, rowTop, kOptionsHighlightHeightNative, textLeft, textRight, Align::Left, argb);
                replay::render::d3d9::TextItem item = {};
                item.x0 = item.clipX0 = static_cast<std::int16_t>(textLeft + kPlayerLabelNative);
                item.x1 = item.clipX1 = static_cast<std::int16_t>(textRight);
                item.y = static_cast<std::int16_t>(rowTop + panelOffset);
                item.h = static_cast<std::int16_t>(kOptionsHighlightHeightNative);
                item.align = Align::Right;
                item.argb = argb;
                item.smallText = true;
                std::snprintf(item.text, sizeof(item.text), "%s", value);
                SubmitFaded(item);
            }
            else if (value != nullptr)
            {
                submit(label, rowTop, kOptionsHighlightHeightNative, textLeft, textRight, Align::Left, argb);
                submit(value, rowTop, kOptionsHighlightHeightNative, textLeft, textRight, Align::Right, argb);
            }
            else
            {
                submit(label, rowTop, kOptionsHighlightHeightNative, textLeft, textRight, Align::Center, argb);
            }
            continue;
        }
        const std::string text = (value != nullptr) ? std::string(label) + ": " + value : std::string(label);
        software(text, rowTop, kOptionsHighlightHeightNative, isSelected ? kOverlayTextClearColorIndex : colorNormal);
    }
}

std::size_t ResolveSelectedVisibleRow(std::uint32_t screenContext, std::size_t rowCount, std::size_t selectedVisibleRow)
{
    if (rowCount == 0)
    {
        return 0;
    }
    if (selectedVisibleRow < rowCount)
    {
        return selectedVisibleRow;
    }

    const std::size_t fallback = static_cast<std::size_t>(ReadByte(screenContext, kOffsetSelectedRow));
    return (fallback < rowCount) ? fallback : 0;
}

bool DrawReplayNameOverlay(std::uint32_t screenContext)
{
    if (screenContext == 0)
    {
        mod::LogThrottled(
            "ReplayHooks::DrawReplayNameOverlay.nullScreen",
            2000,
            "ReplayHooks::DrawReplayNameOverlay: null screen");
        return false;
    }

    g_frameFade = ReadPaletteFade(screenContext);

    std::vector<replay::browser::ReplayBrowserEntry> rows;
    std::size_t selectedVisibleRow = 0;
    std::string folderRelativePath;
    replay::hooks::ReplayOverlayMenuState overlayMenuState = {};
    (void)replay::hooks::GetOverlayMenuState(&overlayMenuState);
    if (!replay::hooks::GetVisibleReplayRows(&rows, &selectedVisibleRow, &folderRelativePath))
    {
        if (ShouldLogEvery(&g_lastOverlayRecoveryTick, 2000))
        {
            mod::Log("ReplayHooks::DrawReplayNameOverlay: rows unavailable -> attempting lazy browser re-init");
        }
        if (!replay::hooks::OnSub770E30InitializeReplayBrowser(screenContext, "replay")
            || !replay::hooks::GetVisibleReplayRows(&rows, &selectedVisibleRow, &folderRelativePath))
        {
            return false;
        }
    }

    if (rows.empty())
    {
        replay::browser::ReplayBrowserEntry placeholder = {};
        placeholder.kind = replay::browser::BrowserEntryKind::ReplayFile;
        placeholder.displayName = replay::i18n::Tr(replay::i18n::Text::NoReplays);
        rows.push_back(placeholder);
        selectedVisibleRow = 0;
    }

    const bool dirty = replay::hooks::ConsumeRowNamesDirtyFlag();
    const std::uint8_t menuState = ReadByte(screenContext, kOffsetMenuState);
    ++g_replayRenderFrameCounter;
    if (dirty || (g_replayRenderFrameCounter % 120ull) == 0ull)
    {
        mod::LogVerbose(
            "ReplayHooks::DrawReplayNameOverlay: frame=%llu folder='%s' rows=%u selectedVisible=%u state=%u selectedRowByte=%u",
            static_cast<unsigned long long>(g_replayRenderFrameCounter),
            folderRelativePath.c_str(),
            static_cast<unsigned>(rows.size()),
            static_cast<unsigned>(selectedVisibleRow),
            static_cast<unsigned>(menuState),
            static_cast<unsigned>(ReadByte(screenContext, kOffsetSelectedRow)));
    }

    const std::size_t rowCount =
        (rows.size() < static_cast<std::size_t>(kOverlayVisibleRowsMax))
        ? rows.size()
        : static_cast<std::size_t>(kOverlayVisibleRowsMax);
    const std::size_t selectedRow = ResolveSelectedVisibleRow(screenContext, rowCount, selectedVisibleRow);
    // Same positions vanilla blits the bars at this frame: see HookedRenderReplayScreenImplInner.
    const double motionAngle = replay::hooks::GetMenuMotionAngle();
    const int rowDrop = replay::motion::RowDrop(motionAngle);
    const int selectedRise = static_cast<int>(replay::motion::SelectedRise(static_cast<int>(selectedRow), motionAngle));
    const auto rowTopNative = [&](std::size_t row) {
        return kOverlayBaseYNative + static_cast<int>(row) * kOverlayRowStepNative
            + (row == selectedRow ? -selectedRise : rowDrop);
    };

    ApplyReplayUiRowLaneTrim(screenContext, rowCount);

    const bool useRt = replay::render::d3d9::IsAvailable();
    if (useRt)
    {
        replay::render::d3d9::BeginFrame();
    }
    if (g_frameFade.darken < 1.0f || g_frameFade.lighten > 0.0f)
    {
        mod::LogVerbose("ReplayHooks::DrawReplayNameOverlay: fade frame darken=%.2f lighten=%.2f rt=%d rows=%u",
            g_frameFade.darken, g_frameFade.lighten, useRt ? 1 : 0, static_cast<unsigned>(rowCount));
    }

    replay::render::LockedMenuSurface locked = {};
    if (!replay::render::AcquireMenuDrawSurfaceLock(screenContext, &locked))
    {
        mod::LogThrottled(
            "ReplayHooks::DrawReplayNameOverlay.surfaceLockUnavailable",
            2000,
            "ReplayHooks::DrawReplayNameOverlay: surface lock unavailable");
        return false;
    }

    const int uiScale = (locked.width >= 1280 && locked.height >= 960) ? 2 : 1;
    const int fontScaleX = uiScale;
    const int fontScaleY = uiScale;
    const int glyphHeight = 7 * fontScaleY;

    std::uint8_t colorSelected = 240;
    std::uint8_t colorNormal = 220;
    replay::render::ResolveOverlayTextPaletteColors(
        screenContext,
        0,
        255,
        &colorSelected,
        &colorNormal);

    const replay::render::IndexedSurfaceView surfaceView = {
        locked.pixels,
        locked.width,
        locked.height,
        locked.pitch,
    };
    const int xDenom = (locked.lockWidth > 0) ? locked.lockWidth : ((locked.width >= 640) ? 640 : 320);
    const int yDenom = (locked.lockHeight > 0) ? locked.lockHeight : ((locked.height >= 480) ? 480 : 240);
    const int leftX = MulDiv(kOverlayLeftXNative, locked.width, xDenom);
    const int rightX = MulDiv(kOverlayRightXNative, locked.width, xDenom);
    int rowStepPx = MulDiv(kOverlayRowStepNative, locked.height, yDenom);
    if (rowStepPx <= 0)
    {
        rowStepPx = kOverlayRowStepNative;
    }
    int rowHeightPx = MulDiv(kOverlayRowHeightNative, locked.height, yDenom);
    if (rowHeightPx <= 0)
    {
        rowHeightPx = kOverlayRowHeightNative;
    }

    if (dirty || ShouldLogEvery(&g_lastOverlayLayoutTick, 2000))
    {
        mod::LogVerbose(
            "ReplayHooks::DrawReplayNameOverlay: layout baseY=%d step=%d rowH=%d textNudgeY=%d left=%d right=%d uiScale=%d swapSelectedStyling=%d surf=%dx%d pitch=%d lock=%dx%d bpp=%d descFlags=0x%08X pfFlags=0x%08X xDenom=%d yDenom=%d",
            kOverlayBaseYNative,
            kOverlayRowStepNative,
            kOverlayRowHeightNative,
            kOverlayTextNativeNudgeY,
            kOverlayLeftXNative,
            kOverlayRightXNative,
            uiScale,
            kOverlaySwapSelectedStyling ? 1 : 0,
            locked.width,
            locked.height,
            locked.pitch,
            locked.lockWidth,
            locked.lockHeight,
            locked.bitsPerPixel,
            static_cast<unsigned>(locked.descFlags),
            static_cast<unsigned>(locked.pixelFormatFlags),
            xDenom,
            yDenom);
    }

    if (kOverlayRewriteRowBackgrounds)
    {
        const int listTopY = MulDiv(kOverlayBaseYNative, locked.height, yDenom);
        const int listBottomY = listTopY + rowStepPx * kOverlayVisibleRowsMax - 1;
        FillIndexedSurfaceRect(surfaceView, leftX, listTopY, rightX, listBottomY, 0);
        DrawMappedReplayBars(
            screenContext,
            surfaceView,
            leftX,
            rightX,
            rowHeightPx,
            locked.height,
            yDenom,
            rowCount,
            selectedRow);
    }

    // Clear text scanlines before redrawing glyphs so motion does not leave stale pixels.
    // Vanilla row sprites use transparency in the label area, so old text can otherwise persist.
    for (std::size_t row = 0; row < rowCount; ++row)
    {
        if (useRt)
        {
            const int laneTop = MulDiv(rowTopNative(row), locked.height, yDenom);
            const int laneBottom = laneTop + MulDiv(kRtTextLaneHeightNative, locked.height, yDenom) - 1;
            FillIndexedSurfaceRect(surfaceView, 0, laneTop, surfaceView.width - 1, laneBottom, kOverlayTextClearColorIndex);
            continue;
        }
        const std::string text = BuildReplayOverlayText(rows[row]);
        const bool nonAscii = ContainsNonAsciiText(text);
        const bool hasJapanese = ContainsJapaneseCodepointUtf8(text);
        const int baseY = rowTopNative(row);
        const int topY = MulDiv(baseY, locked.height, yDenom);
        const int rowHeight = rowHeightPx;
        const int textBaseY = topY + ((rowHeight > glyphHeight) ? ((rowHeight - glyphHeight) / 2) : 0)
            + MulDiv(kOverlayTextNativeNudgeY, locked.height, yDenom);
        const int nonJapaneseLiftPx = hasJapanese ? 0 : MulDiv(kOverlayNonJapaneseLiftNative, locked.height, yDenom);
        const int textY = textBaseY + nonJapaneseLiftPx;
        const int unicodeLaneHeightPx = std::max(1, MulDiv(kOverlayTextLaneHeightNative, locked.height, yDenom));
        const int barTop = MulDiv(baseY, locked.height, yDenom);
        const int barBottom = barTop + MulDiv(kRtTextLaneHeightNative, locked.height, yDenom) - 1;
        const int clearTop = std::max(textY - 1, barTop);
        const int clearBottom = std::min(
            nonAscii ? (textY - 1 + unicodeLaneHeightPx - 1) : (textY + glyphHeight + 1), barBottom);
        FillIndexedSurfaceRect(
            surfaceView,
            0,
            clearTop,
            surfaceView.width - 1,
            clearBottom,
            kOverlayTextClearColorIndex);
    }

    if (overlayMenuState.filterActive && !overlayMenuState.open)
    {
        const char* const first = replay::name::ReplayCharacterName(overlayMenuState.filterCharacters[0]);
        const char* const second = replay::name::ReplayCharacterName(overlayMenuState.filterCharacters[1]);
        std::string what = (first != nullptr && second != nullptr) ? std::string(first) + " vs " + second
            : (first != nullptr)                                   ? first
            : (second != nullptr)                                  ? second
                                                                   : "";
        if (overlayMenuState.filterPlayer[0] != '\0')
        {
            what += what.empty() ? overlayMenuState.filterPlayer : std::string(" + ") + overlayMenuState.filterPlayer;
        }
        replay::render::d3d9::TextItem item = {};
        item.x0 = static_cast<std::int16_t>(kOverlayLeftXNative);
        item.x1 = static_cast<std::int16_t>(kOverlayRightXNative);
        item.y = static_cast<std::int16_t>(kOverlayBaseYNative - kRtTextLaneHeightNative);
        item.h = static_cast<std::int16_t>(kRtTextLaneHeightNative);
        item.align = replay::render::d3d9::TextAlign::Right;
        item.argb = kRtTextSelectedArgb;
        item.smallText = true;
        std::snprintf(item.text, sizeof(item.text), replay::i18n::Tr(replay::i18n::Text::FilterTagFmt), what.c_str(),
            overlayMenuState.shownReplays, overlayMenuState.totalReplays);
        if (useRt)
        {
            SubmitFaded(item);
        }
        else
        {
            replay::render::DrawTextCentered5x7(surfaceView, item.text, MulDiv(kOverlayLeftXNative, locked.width, xDenom),
                MulDiv(kOverlayRightXNative, locked.width, xDenom),
                MulDiv(kOverlayBaseYNative - kRtTextLaneHeightNative, locked.height, yDenom), fontScaleX, fontScaleY,
                colorSelected);
        }
    }

    for (std::size_t row = 0; row < rowCount; ++row)
    {
        const bool isSelected = (row == selectedRow);
        const bool selectedStyling = kOverlaySwapSelectedStyling ? !isSelected : isSelected;
        const int baseY = rowTopNative(row);
        const int drawYNative = baseY;
        if (baseY >= 240 || baseY <= -kOverlayRowStepNative)
        {
            continue;
        }

        const std::string text = BuildReplayOverlayText(rows[row]);
        if (text.empty())
        {
            continue;
        }
        if (useRt)
        {
            replay::render::d3d9::TextItem item = {};
            item.x0 = static_cast<std::int16_t>(kOverlayLeftXNative);
            item.x1 = static_cast<std::int16_t>(kOverlayRightXNative);
            item.y = static_cast<std::int16_t>(baseY);
            item.h = static_cast<std::int16_t>(kRtTextLaneHeightNative);
            item.argb = selectedStyling ? kRtTextSelectedArgb : kRtTextNormalArgb;
            // RT text draws above all software pixels: drop row text under the open panel.
            const int panelOffset = replay::motion::PanelOffset(motionAngle);
            if (overlayMenuState.open && baseY + kRtTextLaneHeightNative > kOptionsPanelTopNative + panelOffset
                && baseY <= kOptionsTipBottomNative + panelOffset)
            {
                continue;
            }
            if (isSelected)
            {
                replay::render::d3d9::TextItem bar = item;
                bar.x0 = 0;
                bar.x1 = 320;
                bar.fillArgb = kRtSelectedBarWashArgb;
                SubmitFaded(bar);
            }
            item.noScroll = !isSelected;
            std::snprintf(item.text, sizeof(item.text), "%s", text.c_str());
            SubmitFaded(item);
            continue;
        }
        const bool hasJapanese = ContainsJapaneseCodepointUtf8(text);

        const int topY = MulDiv(drawYNative, locked.height, yDenom);
        const int rowHeight = rowHeightPx;
        const int textBaseY = topY + ((rowHeight > glyphHeight) ? ((rowHeight - glyphHeight) / 2) : 0)
            + MulDiv(kOverlayTextNativeNudgeY, locked.height, yDenom);
        const int nonJapaneseLiftPx = hasJapanese ? 0 : MulDiv(kOverlayNonJapaneseLiftNative, locked.height, yDenom);
        const int textY = textBaseY + nonJapaneseLiftPx;
        const std::uint8_t color = selectedStyling ? colorSelected : colorNormal;
        if (isSelected)
        {
            // No alpha in the 8-bit surface: outline the selected bar instead of a wash.
            const int barTop = MulDiv(baseY, locked.height, yDenom);
            const int barBottom = barTop + MulDiv(kRtTextLaneHeightNative, locked.height, yDenom) - 1;
            FillIndexedSurfaceRect(surfaceView, leftX, barTop, rightX, barTop, colorNormal);
            FillIndexedSurfaceRect(surfaceView, leftX, barBottom, rightX, barBottom, colorNormal);
        }

        const std::uint32_t checksumBefore = SampleRowChecksum(
            surfaceView,
            leftX,
            rightX,
            topY,
            rowHeight);

        replay::render::DrawTextCentered5x7(
            surfaceView,
            text,
            leftX,
            rightX,
            textY,
            fontScaleX,
            fontScaleY,
            color);

        const std::uint32_t checksumAfter = SampleRowChecksum(
            surfaceView,
            leftX,
            rightX,
            topY,
            rowHeight);
        const bool logRowDiagnostics = kOverlayLogRowDiagnosticsAlways
            || dirty
            || ShouldLogEvery(&g_lastOverlayRowDiagTick, 1000);
        if (logRowDiagnostics)
        {
            const int sampleX = leftX + 4;
            const int sampleYTop = topY + 1;
            const int sampleYMid = topY + (rowHeight / 2);
            const int sampleYBot = topY + rowHeight - 2;
            const std::uint8_t sampleTop = kOverlayVerboseSampling ? SampleIndexedSurfacePixel(surfaceView, sampleX, sampleYTop) : 0;
            const std::uint8_t sampleMid = kOverlayVerboseSampling ? SampleIndexedSurfacePixel(surfaceView, sampleX, sampleYMid) : 0;
            const std::uint8_t sampleBot = kOverlayVerboseSampling ? SampleIndexedSurfacePixel(surfaceView, sampleX, sampleYBot) : 0;
            mod::Log(
                "ReplayHooks::DrawReplayNameOverlay: row=%u selected=%d styleSelected=%d yNative=%d topY=%d rowH=%d textY=%d kind=%d color=%u checksum(before/after)=0x%08X/0x%08X sample(top/mid/bot)=%u/%u/%u text='%s'",
                static_cast<unsigned>(row),
                isSelected ? 1 : 0,
                selectedStyling ? 1 : 0,
                drawYNative,
                topY,
                rowHeight,
                textY,
                static_cast<int>(rows[row].kind),
                static_cast<unsigned>(color),
                static_cast<unsigned>(checksumBefore),
                static_cast<unsigned>(checksumAfter),
                static_cast<unsigned>(sampleTop),
                static_cast<unsigned>(sampleMid),
                static_cast<unsigned>(sampleBot),
                text.c_str());
        }
    }

    DrawOptionsPanel(
        surfaceView,
        xDenom,
        yDenom,
        fontScaleX,
        fontScaleY,
        colorSelected,
        colorNormal,
        overlayMenuState,
        useRt);

    replay::render::ReleaseMenuDrawSurfaceLock(locked);
    if (useRt)
    {
        replay::render::d3d9::CommitFrame();
    }
    return useRt;
}

void PlayUiSoundFromScreen(std::uint32_t screenContext, unsigned short soundIndex)
{
    if (g_playSoundEffect == nullptr)
    {
        mod::Log("ReplayHooks::PlayUiSoundFromScreen: playSoundEffect fn missing");
        return;
    }

    HWND** soundManager = *reinterpret_cast<HWND***>(screenContext + kOffsetSystemPtr);
    if (soundManager == nullptr)
    {
        mod::Log("ReplayHooks::PlayUiSoundFromScreen: soundManager null screen=0x%08X", static_cast<unsigned>(screenContext));
        return;
    }

    (void)g_playSoundEffect(soundManager, soundIndex);
}

// Replaces the replay_bg blit inside renderReplayScreen (thiscall-compatible: ecx =
// graphics context, callee pops the 11 stack args). Draws the 320x240 background as
// two slices wrapped at a time-based offset.
BOOL __fastcall HookedReplayBackgroundBlit(void* graphicsContext, void* /*edx*/, int destX, int destY, int destRight,
    int destBottom, int sourceSurface, int srcX, int srcY, int srcRight, int srcBottom, char transparentColor,
    int flipHorizontal)
{
    const auto blit = reinterpret_cast<BlitSurfaceWithTransparencyFn>(RuntimeAddress(kVaBlitSurfaceWithTransparency));
    const bool fullScreen = destX == 0 && destY == 0 && destRight == 320 && destBottom == 240
        && srcX == 0 && srcY == 0 && srcRight == 320 && srcBottom == 240;
    if (!fullScreen || kReplayBackgroundScrollPixelsPerSecond <= 0.0 || !replay::settings::GetBackgroundScroll())
    {
        return blit(graphicsContext, destX, destY, destRight, destBottom, sourceSurface, srcX, srcY, srcRight,
            srcBottom, transparentColor, flipHorizontal);
    }

    const DWORD now = GetTickCount();
    const DWORD elapsedMs = (g_replayBackgroundScrollTick == 0) ? 0 : std::min<DWORD>(now - g_replayBackgroundScrollTick, 250);
    g_replayBackgroundScrollTick = now;
    g_replayBackgroundScrollOffset = std::fmod(
        g_replayBackgroundScrollOffset + elapsedMs * kReplayBackgroundScrollPixelsPerSecond / 1000.0, 320.0);
    const int scrollX = static_cast<int>(g_replayBackgroundScrollOffset);
    BOOL result = blit(graphicsContext, 0, 0, 320 - scrollX, 240, sourceSurface, scrollX, 0, 320, 240,
        transparentColor, flipHorizontal);
    if (scrollX > 0)
    {
        result = blit(graphicsContext, 320 - scrollX, 0, 320, 240, sourceSurface, 0, 0, scrollX, 240,
            transparentColor, flipHorizontal) && result;
    }
    return result;
}

BOOL __cdecl HookedRenderReplayScreenImplInner(std::uint32_t screenContext)
{
    if (g_originalRenderReplayScreen == nullptr)
    {
        mod::Log("ReplayHooks::HookedRenderReplayScreenImpl: original pointer missing");
        return FALSE;
    }

    if (screenContext == 0)
    {
        return g_originalRenderReplayScreen(screenContext);
    }

    std::uint8_t savedExists[81] = {};
    std::memcpy(savedExists, reinterpret_cast<void*>(screenContext + kOffsetLegacyExistsGrid), sizeof(savedExists));
    std::memset(reinterpret_cast<void*>(screenContext + kOffsetLegacyExistsGrid), 0, sizeof(savedExists));

    // Force menuState to 0 (file-list view) so the vanilla renderer never enters the
    // PLAY REPLAY / RECORD REPLAY submenu-bar draw path regardless of sprite sheet state.
    const std::uint8_t savedMenuState = ReadByte(screenContext, kOffsetMenuState);
    WriteByte(screenContext, kOffsetMenuState, 0);

    // Vanilla moves its rows from these two; its own panel, also on the angle, is blanked in the sheet.
    double& vanillaAngle = *reinterpret_cast<double*>(screenContext + kOffsetAnimPhase);
    double& vanillaRise = *reinterpret_cast<double*>(screenContext + kOffsetAnimRowOffset);
    const double savedAngle = vanillaAngle;
    const double savedRise = vanillaRise;
    vanillaAngle = replay::hooks::GetMenuMotionAngle();
    vanillaRise = replay::motion::SelectedRise(ReadByte(screenContext, kOffsetSelectedRow), vanillaAngle);

    const BOOL result = g_originalRenderReplayScreen(screenContext);

    vanillaAngle = savedAngle;
    vanillaRise = savedRise;
    WriteByte(screenContext, kOffsetMenuState, savedMenuState);
    std::memcpy(reinterpret_cast<void*>(screenContext + kOffsetLegacyExistsGrid), savedExists, sizeof(savedExists));
    return result;
}

BOOL __cdecl HookedRenderReplayScreenImpl(std::uint32_t screenContext)
{
    g_insideReplayRender = true;
    g_activeReplayRenderScreenContext = screenContext;

    BOOL result = FALSE;
#if defined(_MSC_VER)
    __try
    {
        result = HookedRenderReplayScreenImplInner(screenContext);
    }
    __except (crash::HandleSehException("ReplayHooks::HookedRenderReplayScreenImpl", GetExceptionInformation()))
    {
        mod::Log(
            "ReplayHooks::HookedRenderReplayScreenImpl: SEH caught for screen=0x%08X",
            static_cast<unsigned>(screenContext));
        result = FALSE;
    }
#else
    result = HookedRenderReplayScreenImplInner(screenContext);
#endif

    g_activeReplayRenderScreenContext = 0;
    g_insideReplayRender = false;
    return result;
}

BOOL __cdecl HookedPresentFrameToScreenImplInner(std::uint32_t graphicsContext)
{
    if (g_originalPresentFrameToScreen == nullptr)
    {
        mod::LogThrottled(
            "ReplayHooks::HookedPresentFrameToScreenImpl.originalMissing",
            2000,
            "ReplayHooks::HookedPresentFrameToScreenImpl: original pointer missing");
        return FALSE;
    }

    // Runs every frame on every screen: anything but a drawn replay-menu frame drops
    // the RT text so it never lingers over other screens or fades.
    bool drewReplayOverlay = false;
    if (g_insideReplayRender && g_activeReplayRenderScreenContext != 0)
    {
        const std::uint32_t screenContext = g_activeReplayRenderScreenContext;
        const std::uint32_t expectedGraphicsContext = ReadDword(screenContext, kOffsetGraphicsContext);
        if (expectedGraphicsContext == graphicsContext)
        {
            const std::uint8_t menuState = ReadByte(screenContext, kOffsetMenuState);
            const bool shouldDrawOverlay = (menuState == 0 || menuState == 1 || menuState == 3);
            if (shouldDrawOverlay)
            {
                drewReplayOverlay = DrawReplayNameOverlay(screenContext);
            }
        }
        else
        {
            mod::Log(
                "ReplayHooks::HookedPresentFrameToScreenImpl: context mismatch expectedGc=0x%08X got=0x%08X screen=0x%08X",
                static_cast<unsigned>(expectedGraphicsContext),
                static_cast<unsigned>(graphicsContext),
                static_cast<unsigned>(screenContext));
        }
    }

    if (!drewReplayOverlay && !DrawPlaybackFrameIndicator())
    {
        replay::render::d3d9::Clear();
    }
    return g_originalPresentFrameToScreen(graphicsContext);
}

BOOL __cdecl HookedPresentFrameToScreenImpl(std::uint32_t graphicsContext)
{
#if defined(_MSC_VER)
    __try
    {
        return HookedPresentFrameToScreenImplInner(graphicsContext);
    }
    __except (crash::HandleSehException("ReplayHooks::HookedPresentFrameToScreenImpl", GetExceptionInformation()))
    {
        mod::Log(
            "ReplayHooks::HookedPresentFrameToScreenImpl: SEH caught graphics=0x%08X",
            static_cast<unsigned>(graphicsContext));
        return FALSE;
    }
#else
    return HookedPresentFrameToScreenImplInner(graphicsContext);
#endif
}

char __cdecl HookedUpdateReplayScreenLogicImplInner(std::uint32_t screenContext)
{
    if (g_originalUpdateReplayScreenLogic == nullptr)
    {
        mod::LogThrottled(
            "ReplayHooks::HookedUpdateReplayScreenLogicImpl.originalMissing",
            2000,
            "ReplayHooks::HookedUpdateReplayScreenLogicImpl: original pointer missing");
        return 0;
    }

    if (screenContext == 0)
    {
        mod::LogThrottled(
            "ReplayHooks::HookedUpdateReplayScreenLogicImpl.nullScreenContext",
            2000,
            "ReplayHooks::HookedUpdateReplayScreenLogicImpl: null screenContext");
        return g_originalUpdateReplayScreenLogic(screenContext);
    }

    const std::uint8_t firstTime = ReadByte(screenContext, kOffsetFirstTimeFlag);
    const std::uint8_t exitFlag = ReadByte(screenContext, kOffsetExitFlag);
    const std::uint8_t menuState = ReadByte(screenContext, kOffsetMenuState);

    const bool changed = !g_replayLogicStateInitialized
        || firstTime != g_lastReplayLogicFirst
        || exitFlag != g_lastReplayLogicExit
        || menuState != g_lastReplayLogicMenuState;
    if (changed || ShouldLogEvery(&g_lastReplayLogicBeginTick, 1500))
    {
        mod::LogVerbose(
            "ReplayHooks::HookedUpdateReplayScreenLogicImpl: begin screen=0x%08X first=%u exit=%u state=%u changed=%d",
            static_cast<unsigned>(screenContext),
            static_cast<unsigned>(firstTime),
            static_cast<unsigned>(exitFlag),
            static_cast<unsigned>(menuState),
            changed ? 1 : 0);
    }
    g_replayLogicStateInitialized = true;
    g_lastReplayLogicFirst = firstTime;
    g_lastReplayLogicExit = exitFlag;
    g_lastReplayLogicMenuState = menuState;

    if (firstTime > 0)
    {
        ResetReplayRowLaneTrimState();
        const bool preInitOk = replay::hooks::OnSub770E30InitializeReplayBrowser(screenContext, "replay");
        (void)EnsurePresentHookInstalled();
        (void)replay::hooks::EnsureRewindInstalled();
        bool openOverrideReady = EnsureReplayOpenOverrideHookInstalled();
        (void)replay::render::d3d9::EnsureHookInstalled();

        // Vanilla's first-time path runs the blocking entry fade through our render hook, so
        // the menu has to be complete now: its first fade frame shows the trimmed lanes too.
        std::vector<replay::browser::ReplayBrowserEntry> entryRows;
        std::size_t entrySelected = 0;
        std::string entryFolder;
        if (replay::hooks::GetVisibleReplayRows(&entryRows, &entrySelected, &entryFolder))
        {
            ApplyReplayUiRowLaneTrim(screenContext, std::max<std::size_t>(entryRows.size(), 1u));
        }

        const char result = g_originalUpdateReplayScreenLogic(screenContext);
        const std::uint8_t postFirstTime = ReadByte(screenContext, kOffsetFirstTimeFlag);
        bool finalInitOk = preInitOk;
        if (postFirstTime == 0)
        {
            if (!finalInitOk)
            {
                finalInitOk = replay::hooks::OnSub770E30InitializeReplayBrowser(screenContext, "replay");
            }
            if (!openOverrideReady)
            {
                openOverrideReady = EnsureReplayOpenOverrideHookInstalled();
            }
        }
        if (!openOverrideReady)
        {
            mod::Log("ReplayHooks::HookedUpdateReplayScreenLogicImpl: warning openFileReader override hook not available");
        }
        mod::Log(
            "ReplayHooks::HookedUpdateReplayScreenLogicImpl: first-time delegated result=%d postFirst=%u preInit=%d finalInit=%d openOverride=%d",
            static_cast<int>(result),
            static_cast<unsigned>(postFirstTime),
            preInitOk ? 1 : 0,
            finalInitOk ? 1 : 0,
            openOverrideReady ? 1 : 0);
        return result;
    }

    if (menuState == 1 || menuState == 2)
    {
        std::string selectedPath;
        if (replay::hooks::GetSelectedReplayPath(&selectedPath))
        {
            return LaunchReplayNow(screenContext, "state-1/2 submenu bypass");
        }
    }

    if (exitFlag > 0 || menuState == 2 || menuState == 4)
    {
        const char result = g_originalUpdateReplayScreenLogic(screenContext);
        mod::Log(
            "ReplayHooks::HookedUpdateReplayScreenLogicImpl: delegated vanilla result=%d",
            static_cast<int>(result));
        return result;
    }

    if (g_checkReplayExitInput != nullptr && !replay::hooks::IsKeyCaptureBusy())
    {
        (void)g_checkReplayExitInput(screenContext);
    }

    if (ReadByte(screenContext, kOffsetExitFlag) > 0)
    {
        const char result = g_originalUpdateReplayScreenLogic(screenContext);
        mod::Log(
            "ReplayHooks::HookedUpdateReplayScreenLogicImpl: exit requested delegated result=%d",
            static_cast<int>(result));
        return result;
    }

    const std::uint32_t sys = ReadDword(screenContext, kOffsetSystemPtr);
    if (g_processPlayerInput != nullptr && sys != 0)
    {
        g_processPlayerInput(reinterpret_cast<int*>(sys));
    }
    else
    {
        mod::Log(
            "ReplayHooks::HookedUpdateReplayScreenLogicImpl: input process skipped fn=%d sys=0x%08X",
            (g_processPlayerInput != nullptr) ? 1 : 0,
            static_cast<unsigned>(sys));
    }

    if (replay::hooks::TickMenuMotion(screenContext))
    {
        replay::hooks::UpdateReplayInputLatch(screenContext, 0);
        replay::hooks::UpdateReplayInputLatch(screenContext, 1);
        (void)HookedRenderReplayScreenImpl(screenContext);
        return 8;
    }

    replay::hooks::ReplayFlowCallbacks callbacks = {};
    callbacks.playUiSound = &PlayUiSoundFromScreen;

    const bool handledP1 = replay::hooks::OnSub76F060HandleListInput(screenContext, 0, callbacks);
    const bool handledP2 = !handledP1 && replay::hooks::OnSub76F060HandleListInput(screenContext, 1, callbacks);
    replay::hooks::TickRewindKeyCapture(screenContext, callbacks);
    replay::hooks::UpdateReplayInputLatch(screenContext, 0);
    replay::hooks::UpdateReplayInputLatch(screenContext, 1);

    if (replay::hooks::ConsumePendingReplayLaunch())
    {
        return LaunchReplayNow(screenContext, "pending replay launch");
    }

    if (handledP1 || handledP2)
    {
        (void)HookedRenderReplayScreenImpl(screenContext);
        mod::LogThrottled(
            "ReplayHooks::HookedUpdateReplayScreenLogicImpl.listInput",
            1000,
            "ReplayHooks::HookedUpdateReplayScreenLogicImpl: list input handled p1=%d p2=%d",
            handledP1 ? 1 : 0,
            handledP2 ? 1 : 0);
        return 8;
    }

    if (IsEfzRevivalLoaded())
    {
        const char result = g_originalUpdateReplayScreenLogic(screenContext);
        if (ShouldLogEvery(&g_lastReplayLogicIdleTick, 2000))
        {
            mod::LogVerbose(
                "ReplayHooks::HookedUpdateReplayScreenLogicImpl: revival compatibility delegated result=%d screen=0x%08X",
                static_cast<int>(result),
                static_cast<unsigned>(screenContext));
        }
        return result;
    }

    (void)HookedRenderReplayScreenImpl(screenContext);

    if (ShouldLogEvery(&g_lastReplayLogicIdleTick, 2000))
    {
        mod::Log(
            "ReplayHooks::HookedUpdateReplayScreenLogicImpl: list idle screen=0x%08X state=%u selectedRow=%u",
            static_cast<unsigned>(screenContext),
            static_cast<unsigned>(menuState),
            static_cast<unsigned>(ReadByte(screenContext, kOffsetSelectedRow)));
    }
    return 8;
}

char __cdecl HookedUpdateReplayScreenLogicImpl(std::uint32_t screenContext)
{
#if defined(_MSC_VER)
    __try
    {
        return HookedUpdateReplayScreenLogicImplInner(screenContext);
    }
    __except (crash::HandleSehException("ReplayHooks::HookedUpdateReplayScreenLogicImpl", GetExceptionInformation()))
    {
        mod::Log(
            "ReplayHooks::HookedUpdateReplayScreenLogicImpl: SEH caught for screen=0x%08X",
            static_cast<unsigned>(screenContext));
        return 0;
    }
#else
    return HookedUpdateReplayScreenLogicImplInner(screenContext);
#endif
}

int __cdecl HookedOpenFileReaderImplInner(void* fileReaderStruct, const char* filePath)
{
    if (g_originalOpenFileReader == nullptr)
    {
        mod::Log("ReplayHooks::HookedOpenFileReaderImpl: original pointer missing");
        return 0;
    }
    if (fileReaderStruct == nullptr || filePath == nullptr)
    {
        mod::Log(
            "ReplayHooks::HookedOpenFileReaderImpl: invalid args reader=%p path=%p",
            fileReaderStruct,
            filePath);
        return 0;
    }

    OpenFileReaderFn openFileReader = g_originalOpenFileReader;
    const std::uintptr_t openFileReaderAddr = reinterpret_cast<std::uintptr_t>(openFileReader);
    if (!IsExecutableAddress(openFileReaderAddr))
    {
        mod::Log(
            "ReplayHooks::HookedOpenFileReaderImpl: original pointer is not executable ptr=0x%08X",
            static_cast<unsigned>(openFileReaderAddr));
        return 0;
    }

    std::filesystem::path selectedPathFs;
    if (!IsVanillaReplayPath(filePath) || !replay::hooks::GetSelectedReplayPathFs(&selectedPathFs))
    {
        return openFileReader(fileReaderStruct, filePath);
    }

    // Never fall back to the vanilla slot file: that silently plays a different replay.
    replay::hooks::ClearSelectedReplayPath();
    if (!EnsureCreateFileHooksInstalled())
    {
        mod::Log("ReplayHooks::HookedOpenFileReaderImpl: path redirect unavailable, not opening '%s'", PathForLog(selectedPathFs).c_str());
        return 0;
    }

    std::wstring from(filePath, filePath + std::strlen(filePath));
    std::replace(from.begin(), from.end(), L'/', L'\\');
    const std::wstring to = selectedPathFs.wstring();
    t_redirectFrom = &from;
    t_redirectTo = &to;
    const int result = openFileReader(fileReaderStruct, filePath);
    t_redirectFrom = nullptr;
    t_redirectTo = nullptr;
    mod::Log(
        "ReplayHooks::HookedOpenFileReaderImpl: '%s' -> '%s' result=%d",
        filePath,
        PathForLog(selectedPathFs).c_str(),
        result);
    return result;
}

int __cdecl HookedOpenFileReaderImpl(void* fileReaderStruct, const char* filePath)
{
#if defined(_MSC_VER)
    int result = 0;
    __try
    {
        result = HookedOpenFileReaderImplInner(fileReaderStruct, filePath);
    }
    __except (crash::HandleSehException("ReplayHooks::HookedOpenFileReaderImpl", GetExceptionInformation()))
    {
        mod::Log(
            "ReplayHooks::HookedOpenFileReaderImpl: SEH caught reader=%p path='%s'",
            fileReaderStruct,
            (filePath != nullptr) ? filePath : "(null)");
        result = 0;
        // Inner's frame is gone: disarm so later CreateFile calls never read its locals.
        t_redirectFrom = nullptr;
        t_redirectTo = nullptr;
    }
    return result;
#else
    return HookedOpenFileReaderImplInner(fileReaderStruct, filePath);
#endif
}

// Battle screen update (vtable 0x7895DC slot 1). Only observes while folder playback
// is queued. Playback ends through battle+45: 2 = match decided, 1 = exit, set either
// by Esc or by the replay stream's EOF flag (sys+82564 [0]) one frame earlier.
bool g_folderPlaybackEofSeen = false;

char __cdecl HookedUpdateBattleScreenLogicImplInner(std::uint32_t battle)
{
    if (replay::hooks::IsHiddenTick())
    {
        return g_originalUpdateBattleScreenLogic(battle); // a hidden EOF must not latch for folder chaining
    }
    const std::uint32_t sys = ReadDword(battle, kOffsetSystemPtr);
    if (sys == 0)
    {
        return g_originalUpdateBattleScreenLogic(battle);
    }
    if (ReadByte(battle, kOffsetFirstTimeFlag) == 1)
    {
        g_folderPlaybackEofSeen = false;
        g_playbackFrame = 0;
    }
    // Read before the original: it clears ioMode and the EOF flag on its way out.
    // Extended .rep playback runs with ioMode 0 (Revival feeds inputs), so a replay
    // battle is "replay game mode and not recording".
    const bool wasPlayback = ReadByte(sys, kSysGameMode) == kGameModeReplay && ReadByte(sys, kSysReplayIoMode) != 2;
    const std::uint8_t exitBefore = ReadByte(battle, kOffsetExitFlag);
    if (wasPlayback && exitBefore == 0 && ReadDword(sys, kSysReplayFileReader) != 0)
    {
        g_folderPlaybackEofSeen = true;
    }
    const char result = g_originalUpdateBattleScreenLogic(battle);

    // Frame indicator: count battle ticks that advanced a playing replay (paused
    // ticks return early without simulating). Revival feeds "extended" .rep inputs
    // itself and drops ioMode to 0 after frame 1, so gate on the replay game mode
    // (sys+4964 == 5, set by the title) and exclude recording (ioMode 2).
    const bool stillPlaying = ReadByte(sys, kSysGameMode) == kGameModeReplay
        && ReadByte(sys, kSysReplayIoMode) != 2 && result == 3;
    if (stillPlaying && ReadDword(battle, kOffsetBattlePaused) == 0)
    {
        ++g_playbackFrame;
    }
    g_playbackTick = stillPlaying ? GetTickCount() : 0;
    g_playbackBattle = stillPlaying ? battle : 0;

    // Revival-recorded streams end with a synthetic Esc rather than the EOF flag, so
    // a stop at (or a tick before) the trailer's exact length is the replay ending.
    std::uint32_t cursor = g_playbackFrame;
    (void)ReadRevivalReplayFrame(&cursor);
    const replay::file::ReplayLength length = replay::hooks::GetLaunchedReplayLength();
    const bool reachedEnd = length.exact && cursor + 2 >= length.frames;
    const bool cancelled = exitBefore == 1 && !g_folderPlaybackEofSeen && !reachedEnd;
    return replay::hooks::OnReplayBattleUpdateResult(sys, result, wasPlayback, cancelled);
}

// Revival's offline replay session keeps the authoritative replay cursor (frame
// counter at session+8): it stops on Revival's pause/step and follows its checkpoint
// rewind/forward hotkeys. Located per build via the session global and identified
// by its vtable (same identities InGameNetplay's address profiles use).
struct RevivalReplayProfile
{
    std::uint32_t peTimestamp;
    std::uint32_t sessionGlobalRva;
    std::uint32_t replayVtableRva;
};
constexpr RevivalReplayProfile kRevivalReplayProfiles[] = {
    {0x5EA876B0u, 0x000A02CCu, 0x00090428u}, // 1.02e
    {0x5F8C58A3u, 0x000A02CCu, 0x00090428u}, // 1.02f
    {0x6085E718u, 0x000A02CCu, 0x00090428u}, // 1.02f framestepping
    {0x6240CE73u, 0x000A02CCu, 0x00090428u}, // 1.02g
    {0x62929371u, 0x000A02ECu, 0x00090568u}, // 1.02h
    {0x63BF27EAu, 0x000A15F8u, 0x00091658u}, // 1.02i
    {0x6A36A6AEu, 0x0014E980u, 0x0016FF50u}, // 1.02j
    {0x6A6F0F7Bu, 0x0014E980u, 0x0016FC30u}, // 1.02j (2026-08-02 renderer split)
};
constexpr std::uint32_t kReplaySessionFrameOffset = 8;
constexpr std::uint32_t kReplaySessionPausedOffset = 0x18;

bool ReadRevivalReplayFrame(std::uint32_t* outFrame, bool* outPaused)
{
    static const RevivalReplayProfile* profile = nullptr;
    static std::uintptr_t base = 0;
    static bool resolved = false;
    if (!resolved)
    {
        resolved = true;
        base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA("EfzRevival.dll"));
        if (base != 0)
        {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            for (const RevivalReplayProfile& candidate : kRevivalReplayProfiles)
            {
                if (candidate.peTimestamp == nt->FileHeader.TimeDateStamp)
                {
                    profile = &candidate;
                }
            }
            mod::Log("ReplayHooks::ReadRevivalReplayFrame: EfzRevival timestamp=0x%08X profile=%d",
                static_cast<unsigned>(nt->FileHeader.TimeDateStamp), profile != nullptr ? 1 : 0);
        }
    }
    if (profile == nullptr)
    {
        return false;
    }
    const std::uint32_t session = *reinterpret_cast<const std::uint32_t*>(base + profile->sessionGlobalRva);
    if (session == 0 || ReadDword(session, 0) != base + profile->replayVtableRva)
    {
        return false;
    }
    *outFrame = ReadDword(session, kReplaySessionFrameOffset);
    if (outPaused != nullptr)
    {
        *outPaused = ReadByte(session, kReplaySessionPausedOffset) != 0;
    }
    return true;
}

std::uint8_t NearestPaletteIndex(std::uint32_t graphicsContext, int r, int g, int b)
{
    void* palette = reinterpret_cast<void*>(ReadDword(graphicsContext, kGraphicsPaletteOffset));
    if (palette == nullptr)
    {
        return 0;
    }
    using GetEntriesFn = HRESULT(__stdcall*)(void*, DWORD, DWORD, DWORD, std::uint8_t*);
    const auto getEntries = reinterpret_cast<GetEntriesFn>((*reinterpret_cast<void***>(palette))[4]);
    std::uint8_t entries[256 * 4] = {};
    if (FAILED(getEntries(palette, 0, 0, 256, entries)))
    {
        return 0;
    }
    int best = 0;
    long bestDistance = LONG_MAX;
    for (int i = 0; i < 256; ++i)
    {
        const long dr = entries[4 * i] - r;
        const long dg = entries[4 * i + 1] - g;
        const long db = entries[4 * i + 2] - b;
        const long distance = dr * dr + dg * dg + db * db;
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = i;
        }
    }
    return static_cast<std::uint8_t>(best);
}

bool DrawPlaybackFrameIndicator()
{
    if (g_playbackBattle == 0 || GetTickCount() - g_playbackTick > 200u || !replay::settings::GetFrameCounter())
    {
        return false;
    }
    std::uint32_t frame = g_playbackFrame;
    bool paused = false;
    (void)ReadRevivalReplayFrame(&frame, &paused);
    // A playing tick presents before it advances the cursor and a paused one never advances it,
    // so while paused the cursor is one past the frame on screen.
    if (paused && frame > 0)
    {
        --frame;
    }
    const replay::file::ReplayLength length = replay::hooks::GetLaunchedReplayLength();
    char text[48] = {};
    if (length.frames != 0)
    {
        std::snprintf(text, sizeof(text), "%u/%u", static_cast<unsigned>(frame), static_cast<unsigned>(length.frames));
    }
    else
    {
        std::snprintf(text, sizeof(text), "%u", static_cast<unsigned>(frame));
    }
    if (const char* tag = replay::hooks::GetRewindTag())
    {
        const std::size_t used = std::strlen(text);
        std::snprintf(text + used, sizeof(text) - used, " %s", tag);
    }

    if (replay::render::d3d9::IsAvailable())
    {
        replay::render::d3d9::TextItem item = {};
        item.x0 = static_cast<std::int16_t>(kIndicatorCenterX - 200);
        item.x1 = static_cast<std::int16_t>(kIndicatorCenterX + 200);
        item.y = static_cast<std::int16_t>(kIndicatorTopY);
        item.pixelScale = kIndicatorDotSize;
        item.argb = 0xFFFFFFFFu;
        std::snprintf(item.text, sizeof(item.text), "%s", text);
        replay::render::d3d9::BeginFrame();
        replay::render::d3d9::Submit(item);
        replay::render::d3d9::CommitFrame();
        return true;
    }

    replay::render::LockedMenuSurface locked = {};
    if (!replay::render::AcquireMenuDrawSurfaceLock(g_playbackBattle, &locked))
    {
        return false;
    }
    const replay::render::IndexedSurfaceView view = {locked.pixels, locked.width, locked.height, locked.pitch};
    const int scale = std::max(1, static_cast<int>(kIndicatorDotSize / 2.0f + 0.5f));
    const int centerX = kIndicatorCenterX / 2;
    const int y = kIndicatorTopY / 2;
    const int halfWidth = 6 * scale * static_cast<int>(std::strlen(text));
    const std::uint32_t graphicsContext = ReadDword(g_playbackBattle, kOffsetGraphicsContext);
    const std::uint8_t shadow = NearestPaletteIndex(graphicsContext, 0, 0, 0);
    const std::uint8_t ink = NearestPaletteIndex(graphicsContext, 255, 255, 255);
    replay::render::DrawTextCentered5x7(view, text, centerX - halfWidth + 1, centerX + halfWidth + 1, y + 1, scale, scale, shadow);
    replay::render::DrawTextCentered5x7(view, text, centerX - halfWidth, centerX + halfWidth, y, scale, scale, ink);
    replay::render::ReleaseMenuDrawSurfaceLock(locked);
    return false;
}

char __cdecl HookedUpdateBattleScreenLogicImpl(std::uint32_t battle)
{
#if defined(_MSC_VER)
    __try
    {
        return HookedUpdateBattleScreenLogicImplInner(battle);
    }
    __except (crash::HandleSehException("ReplayHooks::HookedUpdateBattleScreenLogicImpl", GetExceptionInformation()))
    {
        mod::Log("ReplayHooks::HookedUpdateBattleScreenLogicImpl: SEH caught battle=0x%08X", static_cast<unsigned>(battle));
        replay::hooks::NoteBattleFault();
        replay::hooks::StopFolderPlayback();
        return 8;
    }
#else
    return HookedUpdateBattleScreenLogicImplInner(battle);
#endif
}

#if defined(_M_IX86)
extern "C" __declspec(naked) void HookedRenderReplayScreenThunk()
{
    __asm
    {
        push ecx
        call HookedRenderReplayScreenImpl
        add esp, 4
        ret
    }
}

extern "C" __declspec(naked) void HookedPresentFrameToScreenThunk()
{
    __asm
    {
        push ecx
        call HookedPresentFrameToScreenImpl
        add esp, 4
        ret
    }
}

extern "C" __declspec(naked) void HookedUpdateReplayScreenLogicThunk()
{
    __asm
    {
        push ecx
        call HookedUpdateReplayScreenLogicImpl
        add esp, 4
        ret
    }
}

extern "C" __declspec(naked) void HookedUpdateBattleScreenLogicThunk()
{
    __asm
    {
        push ecx
        call HookedUpdateBattleScreenLogicImpl
        add esp, 4
        ret
    }
}

extern "C" __declspec(naked) void HookedOpenFileReaderThunk()
{
    __asm
    {
        push dword ptr [esp + 4]
        push ecx
        call HookedOpenFileReaderImpl
        add esp, 8
        ret 4
    }
}
#endif
}

namespace replay::hooks
{
bool IsReplayRedirectReady()
{
    return g_originalOpenFileReader != nullptr && EnsureCreateFileHooksInstalled();
}
}

namespace replay
{
bool InstallHooks()
{
    std::lock_guard<std::mutex> lock(g_hookMutex);
    CleanupAsciiReplayAlias();
    replay::settings::Load();

    if (g_hooksInstalled.load())
    {
        mod::Log("ReplayHooks::InstallHooks: already installed");
        return true;
    }

    if (sizeof(void*) != 4)
    {
        mod::Log("ReplayHooks::InstallHooks: requires x86 process (sizeof(void*)=%u)", static_cast<unsigned>(sizeof(void*)));
        return false;
    }

#if !defined(_M_IX86)
    mod::Log("ReplayHooks::InstallHooks: build target is not x86");
    return false;
#else
    g_exeBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
    if (g_exeBase == 0)
    {
        mod::Log("ReplayHooks::InstallHooks: failed to resolve exe base");
        return false;
    }
    mod::Log("ReplayHooks::InstallHooks: exeBase=0x%08X", static_cast<unsigned>(g_exeBase));
    mod::Log("ReplayHooks::InstallHooks: buildTag=%s", kHookBuildTag);

    g_processPlayerInput = reinterpret_cast<ProcessPlayerInputFn>(RuntimeAddress(kVaProcessPlayerInput));
    g_playSoundEffect = reinterpret_cast<PlaySoundEffectFn>(RuntimeAddress(kVaPlaySoundEffect));
    g_fadeWithSoundAdjustment = reinterpret_cast<FadeWithSoundAdjustmentFn>(RuntimeAddress(kVaFadeWithSoundAdjustment));
    g_checkReplayExitInput = reinterpret_cast<CheckReplayExitInputFn>(RuntimeAddress(kVaCheckReplayExitInput));
    mod::Log(
        "ReplayHooks::InstallHooks: resolved helpers processInput=0x%08X playSound=0x%08X fade=0x%08X checkExit=0x%08X",
        static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(g_processPlayerInput)),
        static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(g_playSoundEffect)),
        static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(g_fadeWithSoundAdjustment)),
        static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(g_checkReplayExitInput)));

    if (kUseDllLocalReplayAssets)
    {
        bool patchedAssets = true;
        patchedAssets = patchedAssets && PatchReplayAssetPathIfPresent(
            0x007896C4,
            "system\\replay_bg.dat",
            "assets\\replay_bg.dat",
            "replay bg path A");
        patchedAssets = patchedAssets && PatchReplayAssetPathIfPresent(
            0x007896DC,
            "system\\replay.dat",
            "assets\\replay.dat",
            "replay obj path A");
        patchedAssets = patchedAssets && PatchReplayAssetPathIfPresent(
            0x007896F0,
            "system\\replay_bg.dat",
            "assets\\replay_bg.dat",
            "replay bg path B");
        patchedAssets = patchedAssets && PatchReplayAssetPathIfPresent(
            0x00789708,
            "system\\replay.dat",
            "assets\\replay.dat",
            "replay obj path B");
        patchedAssets = patchedAssets && PatchReplayAssetPathIfPresent(
            0x00789774,
            "system\\replay.dat",
            "assets\\replay.dat",
            "replay palette path A");
        patchedAssets = patchedAssets && PatchReplayAssetPathIfPresent(
            0x007897B4,
            "system\\replay.dat",
            "assets\\replay.dat",
            "replay palette path B");
        if (!patchedAssets)
        {
            replay::patch::RestorePatches(&g_appliedPatches);
            mod::Log("ReplayHooks::InstallHooks: failed replay asset path patches");
            return false;
        }
    }
    else
    {
        mod::Log("ReplayHooks::InstallHooks: replay asset path patching disabled (using vanilla system paths for modloader override)");
    }

    // Startup only does non-suspending writes (vtable slots, one CALL operand). The
    // MinHook detours, which freeze every other thread, are installed later on the game
    // thread from the replay screen; freezing threads here raced other mods' startup
    // patchers (InGameNetplay's per-frame hook transaction gives up on any interference).
    g_originalRenderReplayScreen =
        reinterpret_cast<RenderReplayScreenFn>(RuntimeAddress(kVaRenderReplayScreen));
    g_originalUpdateReplayScreenLogic =
        reinterpret_cast<UpdateReplayScreenLogicFn>(RuntimeAddress(kVaUpdateReplayScreenLogic));

    std::vector<std::uint8_t> expectedRenderVtable(4);
    const std::uint32_t expectedRenderFn = static_cast<std::uint32_t>(RuntimeAddress(kVaRenderReplayScreen));
    std::memcpy(expectedRenderVtable.data(), &expectedRenderFn, sizeof(expectedRenderFn));
    std::vector<std::uint8_t> patchedRenderVtable(4);
    const std::uint32_t patchedRenderFn = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&HookedRenderReplayScreenThunk));
    std::memcpy(patchedRenderVtable.data(), &patchedRenderFn, sizeof(patchedRenderFn));
    if (!replay::patch::ApplyPatch(
            RuntimeAddress(kVaReplayVtableRender),
            expectedRenderVtable,
            patchedRenderVtable,
            "replay vtable render thunk",
            &g_appliedPatches))
    {
        DisableMinHook(&g_presentFrameToScreenTarget);
        replay::patch::RestorePatches(&g_appliedPatches);
        g_originalPresentFrameToScreen = nullptr;
        g_originalRenderReplayScreen = nullptr;
        g_originalUpdateReplayScreenLogic = nullptr;
        mod::Log("ReplayHooks::InstallHooks: failed replay vtable render patch");
        return false;
    }

    std::vector<std::uint8_t> expectedUpdateVtable(4);
    const std::uint32_t expectedUpdateFn = static_cast<std::uint32_t>(RuntimeAddress(kVaUpdateReplayScreenLogic));
    std::memcpy(expectedUpdateVtable.data(), &expectedUpdateFn, sizeof(expectedUpdateFn));
    std::vector<std::uint8_t> patchedUpdateVtable(4);
    const std::uint32_t patchedUpdateFn = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&HookedUpdateReplayScreenLogicThunk));
    std::memcpy(patchedUpdateVtable.data(), &patchedUpdateFn, sizeof(patchedUpdateFn));
    if (!replay::patch::ApplyPatch(
            RuntimeAddress(kVaReplayVtableUpdate),
            expectedUpdateVtable,
            patchedUpdateVtable,
            "replay vtable update thunk",
            &g_appliedPatches))
    {
        DisableMinHook(&g_presentFrameToScreenTarget);
        replay::patch::RestorePatches(&g_appliedPatches);
        g_originalPresentFrameToScreen = nullptr;
        g_originalRenderReplayScreen = nullptr;
        g_originalUpdateReplayScreenLogic = nullptr;
        mod::Log("ReplayHooks::InstallHooks: failed replay vtable update patch");
        return false;
    }

    // Scrolling background: retarget the replay_bg blit CALL (E8 rel32). Cosmetic, so
    // a mismatch (another mod patched the site) just keeps the static background.
    {
        const std::uintptr_t callSite = RuntimeAddress(kVaReplayBackgroundBlitCall);
        const auto rel32 = [callSite](std::uintptr_t target) {
            const std::int32_t rel = static_cast<std::int32_t>(target - (callSite + 5));
            std::vector<std::uint8_t> bytes(5, 0xE8);
            std::memcpy(bytes.data() + 1, &rel, sizeof(rel));
            return bytes;
        };
        if (!replay::patch::ApplyPatch(
                callSite,
                rel32(RuntimeAddress(kVaBlitSurfaceWithTransparency)),
                rel32(reinterpret_cast<std::uintptr_t>(&HookedReplayBackgroundBlit)),
                "replay background blit call",
                &g_appliedPatches))
        {
            mod::Log("ReplayHooks::InstallHooks: replay background call patch failed; background stays static");
        }
    }

    g_originalUpdateBattleScreenLogic =
        reinterpret_cast<UpdateReplayScreenLogicFn>(RuntimeAddress(kVaUpdateBattleScreenLogic));
    std::vector<std::uint8_t> expectedBattleVtable(4);
    const std::uint32_t expectedBattleFn = static_cast<std::uint32_t>(RuntimeAddress(kVaUpdateBattleScreenLogic));
    std::memcpy(expectedBattleVtable.data(), &expectedBattleFn, sizeof(expectedBattleFn));
    std::vector<std::uint8_t> patchedBattleVtable(4);
    const std::uint32_t patchedBattleFn = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&HookedUpdateBattleScreenLogicThunk));
    std::memcpy(patchedBattleVtable.data(), &patchedBattleFn, sizeof(patchedBattleFn));
    if (!replay::patch::ApplyPatch(
            RuntimeAddress(kVaBattleVtableUpdate),
            expectedBattleVtable,
            patchedBattleVtable,
            "battle vtable update thunk",
            &g_appliedPatches))
    {
        mod::Log("ReplayHooks::InstallHooks: battle vtable update patch failed; folder playback stops after one replay");
    }

    g_hooksInstalled.store(true);
    mod::Log("ReplayHooks::InstallHooks: success (vtable render/update patched, present detour active, openFileReader detour deferred)");
    return true;
#endif
}

void RemoveHooks()
{
    std::lock_guard<std::mutex> lock(g_hookMutex);
    if (!g_hooksInstalled.load())
    {
        mod::Log("ReplayHooks::RemoveHooks: not installed");
        return;
    }

    mod::Log("ReplayHooks::RemoveHooks: begin");
    if (g_createFileHooksInstalled)
    {
        DisableMinHook(&g_createFileWTarget);
        DisableMinHook(&g_createFileATarget);
        g_createFileHooksInstalled = false;
        g_createFileHooksAttempted = false;
    }
    replay::render::d3d9::Shutdown();
    replay::hooks::RemoveRewind();
    DisableMinHook(&g_openFileReaderTarget);
    DisableMinHook(&g_presentFrameToScreenTarget);
    replay::patch::RestorePatches(&g_appliedPatches);

    g_originalOpenFileReader = nullptr;
    g_originalPresentFrameToScreen = nullptr;
    g_originalRenderReplayScreen = nullptr;
    g_originalUpdateReplayScreenLogic = nullptr;
    g_processPlayerInput = nullptr;
    g_playSoundEffect = nullptr;
    g_fadeWithSoundAdjustment = nullptr;
    g_checkReplayExitInput = nullptr;
    g_insideReplayRender = false;
    g_activeReplayRenderScreenContext = 0;
    g_replayRenderFrameCounter = 0;
    g_lastReplayLogicBeginTick = 0;
    g_lastReplayLogicIdleTick = 0;
    g_replayLogicStateInitialized = false;
    g_lastReplayLogicFirst = 0;
    g_lastReplayLogicExit = 0;
    g_lastReplayLogicMenuState = 0;
    g_exeBase = 0;
    CleanupAsciiReplayAlias();

    g_hooksInstalled.store(false);
    mod::Log("ReplayHooks::RemoveHooks: done");
}

bool AreHooksInstalled()
{
    const bool installed = g_hooksInstalled.load();
    mod::Log("ReplayHooks::AreHooksInstalled: %d", installed ? 1 : 0);
    return installed;
}

void CleanupRuntimeArtifacts()
{
    CleanupAsciiReplayAlias();
}

void ShowInProgressMessage(HWND owner)
{
    mod::Log("ReplayHooks::ShowInProgressMessage: owner=0x%p", owner);
    MessageBoxA(
        owner,
        "ImprovedReplayMenu hook loaded.\nDetours installed for replay flow and replay file override.",
        "ImprovedReplayMenu",
        MB_OK | MB_ICONINFORMATION);
}
}
