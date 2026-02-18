#include "replay/hooks/replay_flow.h"

#include "logger.h"
#include "replay/core/i18n.h"
#include "replay/core/key_capture.h"
#include "replay/core/menu_motion.h"
#include "replay/core/replay_file_metadata.h"
#include "replay/core/replay_name_format.h"
#include "replay/core/settings.h"
#include "replay/hooks/rewind.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>

namespace
{
constexpr std::uint32_t kOffsetSystemPtr = 28;
constexpr std::uint32_t kOffsetInputLatchBase = 1102; // +0 for p1, +1 for p2
constexpr std::uint32_t kOffsetMenuState = 1129;
constexpr std::uint32_t kOffsetSelectedRow = 1130;
constexpr std::uint32_t kOffsetSelectedOption = 1131;
constexpr std::uint32_t kOffsetConfirmChoice = 1132;
constexpr std::uint32_t kOffsetLegacyFilter = 1133;
constexpr std::uint32_t kOffsetLegacyExistsGrid = 1134; // 9 * 9 bytes
constexpr std::uint32_t kOffsetLegacyCharGrid = 1215;   // 9 * 18 bytes
constexpr std::uint32_t kOffsetAnimPhase = 1864;
constexpr std::uint32_t kOffsetAnimRowOffset = 1872;
constexpr std::uint32_t kOffsetExitFlag = 45;
constexpr double kMenuMotionDegreesPerTick = 6.0;
enum class MenuMotionEnd
{
    None,
    EnterFolder,
    LeaveFolder,
};

constexpr std::uint32_t kSysInputHorizontal = 12;
constexpr std::uint32_t kSysInputVertical = 14;
constexpr std::uint32_t kSysInputConfirm = 16;
constexpr std::uint32_t kSysInputCancel = 18;
constexpr std::uint32_t kSysInputRecord = 20; // Button C edge
constexpr std::uint32_t kSysInputMenu = 22;   // Button D edge
constexpr std::uint32_t kSysInputMenuHeld = 48; // dword per player: D held (processPlayerInput)

constexpr std::uint32_t kSysReplaySlot = 82561;
constexpr std::uint32_t kSysReplayFilter = 82562;
constexpr std::uint32_t kSysReplayIoMode = 82563;
// Vanilla slot name used for every browser launch; the openFileReader hook swaps the
// path. Not data1-1, which ReplayOpener intercepts by exact name.
constexpr std::uint8_t kLaunchReplayFilter = 8;
constexpr std::uint8_t kLaunchReplaySlot = 8;

struct ReplayFlowState
{
    replay::browser::ReplayBrowser browser = {};
    bool browserReady = false;
    bool rowNamesDirty = false;
    bool pendingReplayLaunch = false;
    bool overlayMenuOpen = false;
    bool keysPage = false;
    bool filterPage = false;
    int captureRow = -1;
    replay::input::KeyCapture capture = {};
    bool escGuard = false;
    char notice[64] = {};
    std::size_t overlayMenuSelectedIndex = 0;
    double motionAngle = 0.0;
    double motionStep = 0.0;
    MenuMotionEnd motionEnd = MenuMotionEnd::None;
    std::string replayRootDirectory;
    std::filesystem::path selectedReplayPathFs;
    std::string selectedReplayPath;
    std::vector<std::filesystem::path> folderPlaybackPaths;
    std::size_t nextFolderReplay = 0;
    bool folderPlaybackActive = false;
    std::string folderPlaybackRelativeFolder;
    std::string restoreRelativeFolder;
    std::filesystem::path restoreEntryAbsPathFs;
    std::filesystem::path pendingRecordAbsPathFs;
};

ReplayFlowState g_replayFlowState = {};
replay::file::ReplayLength g_launchedReplayLength = {};
bool g_menuHeldLast[2] = {false, false};
DWORD g_lastMenuToggleTick = 0;
DWORD g_lastIdleInputLogTick[2] = {0, 0};
DWORD g_lastSkipMenuStateLogTick[2] = {0, 0};
DWORD g_lastActiveInputLogTick[2] = {0, 0};
int g_lastInputLogLatch[2] = {-1, -1};
int g_lastInputLogVertical[2] = {99, 99};
int g_lastInputLogHorizontal[2] = {99, 99};
int g_lastInputLogConfirm[2] = {-1, -1};
int g_lastInputLogCancel[2] = {-1, -1};
int g_lastInputLogRecord[2] = {-1, -1};
int g_lastInputLogMenu[2] = {-1, -1};
int g_lastLatchActive[2] = {-1, -1};
DWORD g_lastLatchHeartbeatTick[2] = {0, 0};
DWORD g_lastRowsSnapshotTick = 0;
DWORD g_lastRowsFailedArgsTick = 0;
std::string g_lastRowsFolder;
std::size_t g_lastRowsStart = static_cast<std::size_t>(-1);
std::size_t g_lastRowsEnd = static_cast<std::size_t>(-1);
std::size_t g_lastRowsSelectedVisible = static_cast<std::size_t>(-1);
std::size_t g_lastRowsCount = static_cast<std::size_t>(-1);

void PlayUiSound(const replay::hooks::ReplayFlowCallbacks& callbacks, std::uint32_t screenContext, unsigned short soundIndex);
void SyncLegacyReplayGridFromBrowser(std::uint32_t screenContext);
inline std::uint8_t& ByteAt(std::uint32_t base, std::uint32_t off);
inline std::uint32_t GetSystemPtr(std::uint32_t screenContext);

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

std::string BuildVanillaReplayRelativePath(std::uint8_t filterIndex, std::uint8_t slotIndex)
{
    // Vanilla naming used by setupBattleStage: replay\data[filter+1]-[slot+1].rep
    return std::string("data")
        + std::to_string(static_cast<unsigned>(filterIndex) + 1u)
        + "-"
        + std::to_string(static_cast<unsigned>(slotIndex) + 1u)
        + ".rep";
}

std::string PathForLog(const std::filesystem::path& path)
{
    const auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
}

bool TryAllocateRecordSlot(std::uint8_t* outSlotIndex, std::uint8_t* outFilterIndex, std::filesystem::path* outAbsolutePath, bool* outWouldOverwrite)
{
    if (outSlotIndex == nullptr || outFilterIndex == nullptr || outAbsolutePath == nullptr)
    {
        return false;
    }

    *outSlotIndex = 0;
    *outFilterIndex = 0;
    outAbsolutePath->clear();
    if (outWouldOverwrite != nullptr)
    {
        *outWouldOverwrite = false;
    }

    std::filesystem::path replayRoot;
    if (g_replayFlowState.browserReady)
    {
        replayRoot = g_replayFlowState.browser.GetRootAbsolutePathFs();
    }
    if (replayRoot.empty())
    {
        replayRoot = std::filesystem::path(g_replayFlowState.replayRootDirectory.empty()
                ? std::string("replay")
                : g_replayFlowState.replayRootDirectory);
    }
    std::error_code ec = {};
    if (!std::filesystem::exists(replayRoot, ec))
    {
        (void)std::filesystem::create_directories(replayRoot, ec);
    }

    for (int filter = 0; filter < 9; ++filter)
    {
        for (int slot = 0; slot < 9; ++slot)
        {
            const std::string relative = BuildVanillaReplayRelativePath(
                static_cast<std::uint8_t>(filter),
                static_cast<std::uint8_t>(slot));
            const std::filesystem::path full = replayRoot / relative;
            ec.clear();
            const bool exists = std::filesystem::exists(full, ec) && !ec;
            if (!exists)
            {
                *outSlotIndex = static_cast<std::uint8_t>(slot);
                *outFilterIndex = static_cast<std::uint8_t>(filter);
                *outAbsolutePath = full;
                mod::Log(
                    "ReplayFlow::TryAllocateRecordSlot: root='%s' selected slot=%u filter=%u path='%s' overwrite=0",
                    PathForLog(replayRoot).c_str(),
                    static_cast<unsigned>(*outSlotIndex),
                    static_cast<unsigned>(*outFilterIndex),
                    PathForLog(*outAbsolutePath).c_str());
                return true;
            }

            ec.clear();
            const bool isRegularFile = std::filesystem::is_regular_file(full, ec) && !ec;
            if (isRegularFile)
            {
                ec.clear();
                const std::uintmax_t fileSizeBytes = std::filesystem::file_size(full, ec);
                if (!ec && fileSizeBytes < 14) // no room for a frame after the header
                {
                    *outSlotIndex = static_cast<std::uint8_t>(slot);
                    *outFilterIndex = static_cast<std::uint8_t>(filter);
                    *outAbsolutePath = full;
                    if (outWouldOverwrite != nullptr)
                    {
                        *outWouldOverwrite = true;
                    }
                    mod::Log(
                        "ReplayFlow::TryAllocateRecordSlot: root='%s' reusing empty slot=%u filter=%u path='%s' size=%llu overwrite=1",
                        PathForLog(replayRoot).c_str(),
                        static_cast<unsigned>(*outSlotIndex),
                        static_cast<unsigned>(*outFilterIndex),
                        PathForLog(*outAbsolutePath).c_str(),
                        static_cast<unsigned long long>(fileSizeBytes));
                    return true;
                }
            }

            // Reuse only slots with parseable-but-invalid replay headers.
            // Do not reuse files that are merely unparseable; preserve user data conservatively.
            replay::file::ReplayHeaderMetadata metadata = {};
            const bool parsed = replay::file::ParseReplayHeaderMetadata(full, &metadata);
            const bool headerInvalid = parsed
                && (!replay::file::IsLikelyValidCharacterId(metadata.p1CharacterId)
                    || !replay::file::IsLikelyValidCharacterId(metadata.p2CharacterId));
            if (headerInvalid)
            {
                *outSlotIndex = static_cast<std::uint8_t>(slot);
                *outFilterIndex = static_cast<std::uint8_t>(filter);
                *outAbsolutePath = full;
                if (outWouldOverwrite != nullptr)
                {
                    *outWouldOverwrite = true;
                }
                mod::Log(
                    "ReplayFlow::TryAllocateRecordSlot: root='%s' reusing invalid slot=%u filter=%u path='%s' parsed=%d chars=(%u,%u) overwrite=1",
                    PathForLog(replayRoot).c_str(),
                    static_cast<unsigned>(*outSlotIndex),
                    static_cast<unsigned>(*outFilterIndex),
                    PathForLog(*outAbsolutePath).c_str(),
                    parsed ? 1 : 0,
                    static_cast<unsigned>(metadata.p1CharacterId),
                    static_cast<unsigned>(metadata.p2CharacterId));
                return true;
            }
        }
    }

    const std::string relative = BuildVanillaReplayRelativePath(0, 0);
    const std::filesystem::path full = replayRoot / relative;
    *outSlotIndex = 0;
    *outFilterIndex = 0;
    *outAbsolutePath = full;
    if (outWouldOverwrite != nullptr)
    {
        *outWouldOverwrite = true;
    }
    mod::Log(
        "ReplayFlow::TryAllocateRecordSlot: root='%s' all slots occupied -> fallback path='%s' overwrite=1",
        PathForLog(replayRoot).c_str(),
        PathForLog(*outAbsolutePath).c_str());
    return true;
}

// Arms setupBattleStage's replay load for path. Returns false when the file is gone
// or no longer parses, so nothing ever falls back to a vanilla slot file.
bool PrepareReplayPath(const std::filesystem::path& path, std::uint32_t systemContext)
{
    if (!replay::hooks::IsReplayRedirectReady())
    {
        mod::Log("ReplayFlow::PrepareReplayPath: path redirect hooks unavailable; not launching");
        return false;
    }
    replay::file::ReplayHeaderMetadata metadata = {};
    if (systemContext == 0
        || !replay::file::ParseReplayHeaderMetadata(path, &metadata)
        || !replay::file::IsLikelyValidCharacterId(metadata.p1CharacterId)
        || !replay::file::IsLikelyValidCharacterId(metadata.p2CharacterId))
    {
        mod::Log("ReplayFlow::PrepareReplayPath: not playable '%s'", PathForLog(path).c_str());
        return false;
    }
    g_launchedReplayLength = replay::file::MeasureReplayLength(path);
    g_replayFlowState.selectedReplayPathFs = path;
    g_replayFlowState.selectedReplayPath = PathForLog(path);
    g_replayFlowState.restoreEntryAbsPathFs = path;
    ByteAt(systemContext, kSysReplaySlot) = kLaunchReplaySlot;
    ByteAt(systemContext, kSysReplayFilter) = kLaunchReplayFilter;
    ByteAt(systemContext, kSysReplayIoMode) = 1;
    return true;
}

bool StartFolderPlayback(std::uint32_t screenContext);

void LeaveKeysPage()
{
    g_replayFlowState.keysPage = false;
    g_replayFlowState.filterPage = false;
    g_replayFlowState.captureRow = -1;
    g_replayFlowState.capture.active = false;
    g_replayFlowState.notice[0] = '\0';
}

void BeginMenuMotion(double step, MenuMotionEnd end)
{
    g_replayFlowState.motionStep = step;
    g_replayFlowState.motionEnd = end;
}

void CloseOverlayMenu(const char* reason)
{
    const bool wasOpen = g_replayFlowState.overlayMenuOpen;
    g_replayFlowState.overlayMenuOpen = false;
    g_replayFlowState.motionAngle = 0.0;
    BeginMenuMotion(0.0, MenuMotionEnd::None);
    g_replayFlowState.overlayMenuSelectedIndex = 0;
    LeaveKeysPage();
    if (wasOpen)
    {
        mod::Log(
            "ReplayFlow::OverlayMenu: close reason='%s'",
            (reason != nullptr) ? reason : "(null)");
    }
}

void OpenOverlayMenu()
{
    if (!g_replayFlowState.overlayMenuOpen)
    {
        g_replayFlowState.overlayMenuOpen = true;
        g_replayFlowState.overlayMenuSelectedIndex = 0;
        BeginMenuMotion(kMenuMotionDegreesPerTick, MenuMotionEnd::None);
        mod::Log("ReplayFlow::OverlayMenu: open");
    }
}

bool BeginRecordReplay(std::uint32_t screenContext, const replay::hooks::ReplayFlowCallbacks& callbacks, int playerIndex, const char* reason)
{
    const std::uint32_t sys = GetSystemPtr(screenContext);
    if (sys == 0)
    {
        mod::Log(
            "ReplayFlow::BeginRecordReplay: failed (sys ptr null) player=%d reason='%s'",
            playerIndex,
            (reason != nullptr) ? reason : "(null)");
        return false;
    }

    std::uint8_t recordSlot = 0;
    std::uint8_t recordFilter = 0;
    std::filesystem::path recordPath;
    bool wouldOverwrite = false;
    if (!TryAllocateRecordSlot(&recordSlot, &recordFilter, &recordPath, &wouldOverwrite))
    {
        mod::Log(
            "ReplayFlow::BeginRecordReplay: failed allocate slot player=%d reason='%s'",
            playerIndex,
            (reason != nullptr) ? reason : "(null)");
        return false;
    }

    ByteAt(sys, kSysReplaySlot) = recordSlot;
    ByteAt(sys, kSysReplayFilter) = recordFilter;
    ByteAt(sys, kSysReplayIoMode) = 2; // Record mode.
    ByteAt(screenContext, kOffsetSelectedRow) = recordSlot;
    ByteAt(screenContext, kOffsetLegacyFilter) = recordFilter;
    ByteAt(screenContext, kOffsetExitFlag) = 2;
    g_replayFlowState.selectedReplayPathFs.clear();
    g_replayFlowState.selectedReplayPath.clear();
    g_replayFlowState.pendingReplayLaunch = false;
    g_replayFlowState.pendingRecordAbsPathFs = recordPath;
    CloseOverlayMenu("record launch");
    PlayUiSound(callbacks, screenContext, 6u);

    mod::Log(
        "ReplayFlow::BeginRecordReplay: player=%d slot=%u filter=%u ioMode=%u path='%s' overwrite=%d reason='%s'",
        playerIndex,
        static_cast<unsigned>(ByteAt(sys, kSysReplaySlot)),
        static_cast<unsigned>(ByteAt(sys, kSysReplayFilter)),
        static_cast<unsigned>(ByteAt(sys, kSysReplayIoMode)),
        PathForLog(recordPath).c_str(),
        wouldOverwrite ? 1 : 0,
        (reason != nullptr) ? reason : "(null)");
    return true;
}

bool StepReplaySortOrder(std::uint32_t screenContext, int step)
{
    using replay::browser::ReplaySortOrder;
    static constexpr ReplaySortOrder kOrders[] = {ReplaySortOrder::NewestFirst, ReplaySortOrder::OldestFirst,
        ReplaySortOrder::PlayerName, ReplaySortOrder::LargestFirst};
    const ReplaySortOrder current = g_replayFlowState.browser.GetReplaySortOrder();
    int position = 0;
    for (int i = 0; i < 4; ++i)
    {
        position = (kOrders[i] == current) ? i : position;
    }
    const ReplaySortOrder next = kOrders[(position + (step > 0 ? 1 : 3)) % 4];
    if (!g_replayFlowState.browser.SetReplaySortOrder(next))
    {
        return false;
    }

    SyncLegacyReplayGridFromBrowser(screenContext);
    g_replayFlowState.rowNamesDirty = true;
    replay::settings::SetSortOrder(next);
    mod::Log("ReplayFlow::OverlayMenu: sort -> %d (persisted)", static_cast<int>(next));
    return true;
}

void ApplyReplayFilter(std::uint32_t screenContext, const replay::browser::ReplayBrowserFilter& filter)
{
    g_replayFlowState.browser.SetFilter(filter);
    SyncLegacyReplayGridFromBrowser(screenContext);
    g_replayFlowState.rowNamesDirty = true;
    const replay::browser::ReplayBrowserView* view = g_replayFlowState.browser.CurrentView();
    mod::Log("ReplayFlow::OverlayMenu: filter characters=%d,%d player='%s' shown=%u of %u", filter.characters[0],
        filter.characters[1], filter.player.c_str(), view != nullptr ? static_cast<unsigned>(view->shownReplays) : 0u,
        view != nullptr ? static_cast<unsigned>(view->totalReplays) : 0u);
}

void StepPlayerFilter(std::uint32_t screenContext, int step)
{
    replay::browser::ReplayBrowserFilter filter = g_replayFlowState.browser.GetFilter();
    const std::vector<std::string> players = g_replayFlowState.browser.PlayerOptions();
    const int count = static_cast<int>(players.size());
    int position = count;
    for (int i = 0; i < count; ++i)
    {
        position = (players[i] == filter.player) ? i : position;
    }
    if (position == count && !filter.player.empty())
    {
        filter.player.clear();
    }
    else
    {
        position = (position + (step > 0 ? 1 : count)) % (count + 1);
        filter.player = (position == count) ? std::string() : players[position];
    }
    ApplyReplayFilter(screenContext, filter);
}

inline std::uint8_t& ByteAt(std::uint32_t base, std::uint32_t off)
{
    return *reinterpret_cast<std::uint8_t*>(base + off);
}

inline double& DoubleAt(std::uint32_t base, std::uint32_t off)
{
    return *reinterpret_cast<double*>(base + off);
}

inline std::uint32_t GetSystemPtr(std::uint32_t screenContext)
{
    return *reinterpret_cast<std::uint32_t*>(screenContext + kOffsetSystemPtr);
}

inline std::int8_t ReadInputAxis(std::uint32_t screenContext, int playerIndex, std::uint32_t axisOffset)
{
    const std::uint32_t sys = GetSystemPtr(screenContext);
    if (sys == 0)
    {
        return 0;
    }
    return *reinterpret_cast<std::int8_t*>(sys + axisOffset + static_cast<std::uint32_t>(playerIndex));
}

inline std::uint8_t ReadInputButton(std::uint32_t screenContext, int playerIndex, std::uint32_t buttonOffset)
{
    const std::uint32_t sys = GetSystemPtr(screenContext);
    if (sys == 0)
    {
        return 0;
    }
    return *reinterpret_cast<std::uint8_t*>(sys + buttonOffset + static_cast<std::uint32_t>(playerIndex));
}

void PlayUiSound(const replay::hooks::ReplayFlowCallbacks& callbacks, std::uint32_t screenContext, unsigned short soundIndex)
{
    mod::LogVerbose(
        "ReplayFlow::PlayUiSound: screen=0x%08X sound=%u callback=%d",
        static_cast<unsigned>(screenContext),
        static_cast<unsigned>(soundIndex),
        (callbacks.playUiSound != nullptr) ? 1 : 0);
    if (callbacks.playUiSound != nullptr)
    {
        callbacks.playUiSound(screenContext, soundIndex);
    }
}

void SyncLegacyReplayGridFromBrowser(std::uint32_t screenContext)
{
    mod::LogVerbose(
        "ReplayFlow::SyncLegacyReplayGridFromBrowser: begin screen=0x%08X browserReady=%d",
        static_cast<unsigned>(screenContext),
        g_replayFlowState.browserReady ? 1 : 0);

    for (int i = 0; i < 81; ++i)
    {
        ByteAt(screenContext, kOffsetLegacyExistsGrid + static_cast<std::uint32_t>(i)) = 0;
    }
    for (int i = 0; i < 162; ++i)
    {
        ByteAt(screenContext, kOffsetLegacyCharGrid + static_cast<std::uint32_t>(i)) = 0;
    }

    g_replayFlowState.browser.EnsureVisibleReplayMetadataLoaded();
    const replay::browser::ReplayBrowserView* view = g_replayFlowState.browser.CurrentView();
    if (view == nullptr)
    {
        ByteAt(screenContext, kOffsetSelectedRow) = 0;
        ByteAt(screenContext, kOffsetLegacyFilter) = 0;
        mod::Log("ReplayFlow::SyncLegacyReplayGridFromBrowser: no current view");
        return;
    }

    const std::size_t start = g_replayFlowState.browser.VisibleStartIndex();
    const std::size_t end = g_replayFlowState.browser.VisibleEndIndexExclusive();
    std::size_t localRow = 0;
    std::size_t playableRows = 0;
    for (std::size_t i = start; i < end && localRow < 9; ++i, ++localRow)
    {
        const replay::browser::ReplayBrowserEntry& e = view->entries[i];
        const std::uint32_t existsOff = kOffsetLegacyExistsGrid + static_cast<std::uint32_t>(localRow);
        // Vanilla replay renderer draws legacy character labels when this flag is set.
        // Keep it enabled only for playable replay rows to avoid stale/default names
        // (e.g. RUMI/RUMI) on directories or unavailable entries.
        const bool legacyReplayPresent = e.playable && e.metadata.valid;
        ByteAt(screenContext, existsOff) = legacyReplayPresent ? 1 : 0;
        playableRows += e.playable ? 1 : 0;

        if (!legacyReplayPresent)
        {
            continue;
        }

        const std::uint8_t p1 = static_cast<std::uint8_t>(e.metadata.p1CharacterId & 0xFFu);
        const std::uint8_t p2 = static_cast<std::uint8_t>(e.metadata.p2CharacterId & 0xFFu);
        const std::uint32_t charBase = kOffsetLegacyCharGrid + static_cast<std::uint32_t>(2 * localRow);
        ByteAt(screenContext, charBase + 0) = p1;
        ByteAt(screenContext, charBase + 1) = p2;
    }

    ByteAt(screenContext, kOffsetSelectedRow) = static_cast<std::uint8_t>(g_replayFlowState.browser.SelectedVisibleRow());
    ByteAt(screenContext, kOffsetLegacyFilter) = 0;
    mod::LogVerbose(
        "ReplayFlow::SyncLegacyReplayGridFromBrowser: done folder='%s' start=%u end=%u selectedVisibleRow=%u playableRows=%u",
        view->relativeFolderPath.c_str(),
        static_cast<unsigned>(start),
        static_cast<unsigned>(end),
        static_cast<unsigned>(ByteAt(screenContext, kOffsetSelectedRow)),
        static_cast<unsigned>(playableRows));
}

bool StartFolderPlayback(std::uint32_t screenContext)
{
    replay::hooks::StopFolderPlayback();
    const replay::browser::ReplayBrowserView* view = g_replayFlowState.browser.CurrentView();
    const std::uint32_t sys = GetSystemPtr(screenContext);
    if (!g_replayFlowState.browserReady || view == nullptr || sys == 0)
    {
        return false;
    }
    for (const replay::browser::ReplayBrowserEntry& entry : view->entries)
    {
        // Only rows already on screen have been read. PrepareReplayPath checks the rest in their turn.
        if (entry.kind == replay::browser::BrowserEntryKind::ReplayFile && (!entry.metadataLoaded || entry.playable))
        {
            g_replayFlowState.folderPlaybackPaths.push_back(entry.absolutePathFs);
        }
    }
    g_replayFlowState.folderPlaybackRelativeFolder = view->relativeFolderPath;
    g_replayFlowState.folderPlaybackActive = true;
    mod::Log("ReplayFlow::StartFolderPlayback: folder='%s' replays=%zu",
        view->relativeFolderPath.c_str(), g_replayFlowState.folderPlaybackPaths.size());
    if (!replay::hooks::ContinueFolderPlayback(sys))
    {
        return false;
    }
    ByteAt(screenContext, kOffsetLegacyFilter) = 0;
    g_replayFlowState.pendingReplayLaunch = true;
    CloseOverlayMenu("folder playback");
    return true;
}
}

namespace replay::hooks
{
replay::file::ReplayLength GetLaunchedReplayLength()
{
    return g_launchedReplayLength;
}

bool IsFolderPlaybackActive()
{
    return g_replayFlowState.folderPlaybackActive;
}

void StopFolderPlayback()
{
    if (g_replayFlowState.folderPlaybackActive)
    {
        mod::Log("ReplayFlow::StopFolderPlayback: stopped at %zu/%zu",
            g_replayFlowState.nextFolderReplay, g_replayFlowState.folderPlaybackPaths.size());
    }
    g_replayFlowState.folderPlaybackActive = false;
    g_replayFlowState.folderPlaybackPaths.clear();
    g_replayFlowState.nextFolderReplay = 0;
}

bool ContinueFolderPlayback(std::uint32_t systemContext)
{
    while (g_replayFlowState.folderPlaybackActive
        && g_replayFlowState.nextFolderReplay < g_replayFlowState.folderPlaybackPaths.size())
    {
        const std::filesystem::path path = g_replayFlowState.folderPlaybackPaths[g_replayFlowState.nextFolderReplay++];
        if (PrepareReplayPath(path, systemContext))
        {
            g_replayFlowState.restoreRelativeFolder = g_replayFlowState.folderPlaybackRelativeFolder;
            mod::Log("ReplayFlow::ContinueFolderPlayback: %zu/%zu '%s'", g_replayFlowState.nextFolderReplay,
                g_replayFlowState.folderPlaybackPaths.size(), PathForLog(path).c_str());
            return true;
        }
    }
    StopFolderPlayback();
    return false;
}

char OnReplayBattleUpdateResult(std::uint32_t systemContext, char nextScreen, bool wasPlayback, bool cancelled)
{
    // A replay battle ends with 8 (native .rep: back to the replay menu) or, when
    // Revival fed an extended .rep (ioMode 0), with 5 (match decided) / 1 (exit).
    const bool battleEnded = nextScreen == 8 || nextScreen == 5 || nextScreen == 1;
    if (!g_replayFlowState.folderPlaybackActive || !wasPlayback || !battleEnded)
    {
        return nextScreen;
    }
    if (cancelled)
    {
        StopFolderPlayback();
        return nextScreen;
    }
    // 2 = loading screen, which runs setupBattleStage for the re-armed replay.
    return ContinueFolderPlayback(systemContext) ? 2 : nextScreen;
}

bool OnSub770E30InitializeReplayBrowser(std::uint32_t screenContext, const std::string& replayRootDirectory)
{
    mod::Log(
        "ReplayFlow::OnSub770E30InitializeReplayBrowser: begin screen=0x%08X root='%s'",
        static_cast<unsigned>(screenContext),
        replayRootDirectory.c_str());

    StopFolderPlayback();
    g_replayFlowState.replayRootDirectory = replayRootDirectory;
    g_replayFlowState.selectedReplayPathFs.clear();
    g_replayFlowState.selectedReplayPath.clear();
    g_replayFlowState.pendingReplayLaunch = false;
    g_replayFlowState.overlayMenuOpen = false;
    g_replayFlowState.overlayMenuSelectedIndex = 0;
    LeaveKeysPage();
    g_replayFlowState.motionAngle = 0.0;
    BeginMenuMotion(0.0, MenuMotionEnd::None);

    replay::browser::ReplayBrowserConfig cfg = {};
    cfg.visibleRows = 9;
    cfg.prettifyConcreteNames = true;
    cfg.ignoreUnusableReplayFiles = true;
    cfg.sortDirectoriesFirst = true;
    cfg.replaySortOrder = replay::settings::GetSortOrder();

    try
    {
        g_replayFlowState.browserReady = g_replayFlowState.browser.Initialize(replayRootDirectory, cfg);
    }
    catch (const std::exception& ex)
    {
        g_replayFlowState.browserReady = false;
        mod::Log(
            "ReplayFlow::OnSub770E30InitializeReplayBrowser: exception root='%s' what='%s'",
            replayRootDirectory.c_str(),
            ex.what());
    }
    catch (...)
    {
        g_replayFlowState.browserReady = false;
        mod::Log(
            "ReplayFlow::OnSub770E30InitializeReplayBrowser: unknown exception root='%s'",
            replayRootDirectory.c_str());
    }
    if (!g_replayFlowState.browserReady)
    {
        mod::Log("OnSub770E30InitializeReplayBrowser: initialize failed root='%s'", replayRootDirectory.c_str());
        return false;
    }

    ByteAt(screenContext, kOffsetMenuState) = 0;
    ByteAt(screenContext, kOffsetSelectedOption) = 0;
    ByteAt(screenContext, kOffsetConfirmChoice) = 0;
    ByteAt(screenContext, kOffsetLegacyFilter) = 0;
    DoubleAt(screenContext, kOffsetAnimPhase) = 0.0;
    DoubleAt(screenContext, kOffsetAnimRowOffset) = 0.0;

    if (!g_replayFlowState.pendingRecordAbsPathFs.empty())
    {
        const std::filesystem::path recordedPath = g_replayFlowState.pendingRecordAbsPathFs;
        g_replayFlowState.pendingRecordAbsPathFs.clear();
        mod::Log(
            "ReplayFlow::OnSub770E30InitializeReplayBrowser: restoring to recorded file path='%s'",
            PathForLog(recordedPath).c_str());
        g_replayFlowState.browser.NavigateToAndSelect("", recordedPath);
    }
    else if (!g_replayFlowState.restoreEntryAbsPathFs.empty())
    {
        const std::string folder = g_replayFlowState.restoreRelativeFolder;
        const std::filesystem::path entry = g_replayFlowState.restoreEntryAbsPathFs;
        g_replayFlowState.restoreRelativeFolder.clear();
        g_replayFlowState.restoreEntryAbsPathFs.clear();
        mod::Log(
            "ReplayFlow::OnSub770E30InitializeReplayBrowser: restoring to folder='%s' entry='%s'",
            folder.c_str(),
            PathForLog(entry).c_str());
        g_replayFlowState.browser.NavigateToAndSelect(folder, entry);
    }

    SyncLegacyReplayGridFromBrowser(screenContext);
    g_replayFlowState.rowNamesDirty = true;

    mod::Log(
        "ReplayFlow::OnSub770E30InitializeReplayBrowser: ready root='%s' selectedRow=%u state=%u option=%u confirm=%u filter=%u sort=%s",
        replayRootDirectory.c_str(),
        static_cast<unsigned>(ByteAt(screenContext, kOffsetSelectedRow)),
        static_cast<unsigned>(ByteAt(screenContext, kOffsetMenuState)),
        static_cast<unsigned>(ByteAt(screenContext, kOffsetSelectedOption)),
        static_cast<unsigned>(ByteAt(screenContext, kOffsetConfirmChoice)),
        static_cast<unsigned>(ByteAt(screenContext, kOffsetLegacyFilter)),
        (cfg.replaySortOrder == replay::browser::ReplaySortOrder::NewestFirst) ? "NewestFirst"
            : (cfg.replaySortOrder == replay::browser::ReplaySortOrder::OldestFirst) ? "OldestFirst"
            : (cfg.replaySortOrder == replay::browser::ReplaySortOrder::PlayerName) ? "PlayerName"
                                                                                    : "LargestFirst");
    return true;
}

bool RefreshCurrentReplayFolder(std::uint32_t screenContext)
{
    mod::Log(
        "ReplayFlow::RefreshCurrentReplayFolder: begin screen=0x%08X browserReady=%d",
        static_cast<unsigned>(screenContext),
        g_replayFlowState.browserReady ? 1 : 0);
    if (!g_replayFlowState.browserReady)
    {
        mod::Log("ReplayFlow::RefreshCurrentReplayFolder: skipped (browser not ready)");
        return false;
    }

    bool refreshed = false;
    try
    {
        refreshed = g_replayFlowState.browser.RefreshCurrentFolder();
    }
    catch (const std::exception& ex)
    {
        mod::Log(
            "ReplayFlow::RefreshCurrentReplayFolder: exception what='%s'",
            ex.what());
        refreshed = false;
    }
    catch (...)
    {
        mod::Log("ReplayFlow::RefreshCurrentReplayFolder: unknown exception");
        refreshed = false;
    }

    if (!refreshed)
    {
        mod::Log("ReplayFlow::RefreshCurrentReplayFolder: browser refresh failed");
        return false;
    }

    SyncLegacyReplayGridFromBrowser(screenContext);
    g_replayFlowState.rowNamesDirty = true;
    mod::Log("ReplayFlow::RefreshCurrentReplayFolder: success rowNamesDirty=1");
    return true;
}

bool OnSub76F060HandleListInput(std::uint32_t screenContext, int playerIndex, const ReplayFlowCallbacks& callbacks)
{
    if (!g_replayFlowState.browserReady)
    {
        mod::Log("ReplayFlow::OnSub76F060HandleListInput: skip (browser not ready)");
        return false;
    }
    if (playerIndex < 0 || playerIndex > 1)
    {
        mod::Log("ReplayFlow::OnSub76F060HandleListInput: skip (invalid player index=%d)", playerIndex);
        return false;
    }

    const std::uint8_t menuState = ByteAt(screenContext, kOffsetMenuState);
    if (menuState != 0)
    {
        if (ShouldLogEvery(&g_lastSkipMenuStateLogTick[playerIndex], 1000))
        {
            mod::Log(
                "ReplayFlow::OnSub76F060HandleListInput: skip (menuState=%u != 0) player=%d",
                static_cast<unsigned>(menuState),
                playerIndex);
        }
        return false;
    }

    bool handled = false;
    const bool latch = ByteAt(screenContext, kOffsetInputLatchBase + static_cast<std::uint32_t>(playerIndex)) != 0;
    const std::int8_t vertical = ReadInputAxis(screenContext, playerIndex, kSysInputVertical);
    const std::int8_t horizontal = ReadInputAxis(screenContext, playerIndex, kSysInputHorizontal);
    const std::uint8_t confirm = ReadInputButton(screenContext, playerIndex, kSysInputConfirm);
    const std::uint8_t cancel = ReadInputButton(screenContext, playerIndex, kSysInputCancel);
    const std::uint8_t record = ReadInputButton(screenContext, playerIndex, kSysInputRecord);
    // D toggles the Options menu once per physical press: our own edge on the held
    // level processPlayerInput keeps at sys+48+4*player, plus a short cooldown so a
    // pulsing input layer (or both players sharing a key) cannot re-toggle it.
    const std::uint32_t inputSys = GetSystemPtr(screenContext);
    const bool menuHeld = inputSys != 0
        && *reinterpret_cast<const std::uint32_t*>(inputSys + kSysInputMenuHeld + 4u * static_cast<std::uint32_t>(playerIndex)) != 0;
    const DWORD now = GetTickCount();
    const std::uint8_t menu = (menuHeld && !g_menuHeldLast[playerIndex] && now - g_lastMenuToggleTick >= 200u) ? 1 : 0;
    g_menuHeldLast[playerIndex] = menuHeld;
    if (menu != 0)
    {
        g_lastMenuToggleTick = now;
    }
    const bool anyInput = (vertical != 0) || (horizontal != 0) || (confirm != 0) || (cancel != 0) || (record != 0) || (menu != 0);
    const bool idleSample = !anyInput && !latch;
    const int latchInt = latch ? 1 : 0;
    const int verticalInt = static_cast<int>(vertical);
    const int horizontalInt = static_cast<int>(horizontal);
    const int confirmInt = (confirm != 0) ? 1 : 0;
    const int cancelInt = (cancel != 0) ? 1 : 0;
    const int recordInt = (record != 0) ? 1 : 0;
    const int menuInt = (menu != 0) ? 1 : 0;
    const bool inputStateChanged = (g_lastInputLogLatch[playerIndex] != latchInt)
        || (g_lastInputLogVertical[playerIndex] != verticalInt)
        || (g_lastInputLogHorizontal[playerIndex] != horizontalInt)
        || (g_lastInputLogConfirm[playerIndex] != confirmInt)
        || (g_lastInputLogCancel[playerIndex] != cancelInt)
        || (g_lastInputLogRecord[playerIndex] != recordInt)
        || (g_lastInputLogMenu[playerIndex] != menuInt);
    const bool activeSample = anyInput || latch;
    const bool logIdleHeartbeat = idleSample && ShouldLogEvery(&g_lastIdleInputLogTick[playerIndex], 2000);
    const bool logActiveHeartbeat = activeSample && ShouldLogEvery(&g_lastActiveInputLogTick[playerIndex], 1500);
    const bool logThisFrame = inputStateChanged || logIdleHeartbeat || logActiveHeartbeat;
    g_lastInputLogLatch[playerIndex] = latchInt;
    g_lastInputLogVertical[playerIndex] = verticalInt;
    g_lastInputLogHorizontal[playerIndex] = horizontalInt;
    g_lastInputLogConfirm[playerIndex] = confirmInt;
    g_lastInputLogCancel[playerIndex] = cancelInt;
    g_lastInputLogRecord[playerIndex] = recordInt;
    g_lastInputLogMenu[playerIndex] = menuInt;
    if (logThisFrame)
    {
        mod::LogVerbose(
            "ReplayFlow::OnSub76F060HandleListInput: begin screen=0x%08X player=%d browserReady=%d state=%u latch=%d v=%d h=%d confirm=%u cancel=%u record=%u menu=%u overlayOpen=%d overlaySel=%u",
            static_cast<unsigned>(screenContext),
            playerIndex,
            g_replayFlowState.browserReady ? 1 : 0,
            static_cast<unsigned>(menuState),
            latch ? 1 : 0,
            static_cast<int>(vertical),
            static_cast<int>(horizontal),
            static_cast<unsigned>(confirm),
            static_cast<unsigned>(cancel),
            static_cast<unsigned>(record),
            static_cast<unsigned>(menu),
            g_replayFlowState.overlayMenuOpen ? 1 : 0,
            static_cast<unsigned>(g_replayFlowState.overlayMenuSelectedIndex));
    }

    if (g_replayFlowState.captureRow >= 0 || g_replayFlowState.escGuard)
    {
        return true;
    }

    try
    {
        if (menu != 0)
        {
            if (g_replayFlowState.keysPage)
            {
                LeaveKeysPage();
                g_replayFlowState.overlayMenuSelectedIndex = static_cast<std::size_t>(OptionsItem::RewindKeys);
                PlayUiSound(callbacks, screenContext, 8u);
                handled = true;
            }
            else if (g_replayFlowState.filterPage)
            {
                LeaveKeysPage();
                g_replayFlowState.overlayMenuSelectedIndex = static_cast<std::size_t>(OptionsItem::Sort);
                PlayUiSound(callbacks, screenContext, 8u);
                handled = true;
            }
            else if (g_replayFlowState.overlayMenuOpen)
            {
                BeginMenuMotion(-kMenuMotionDegreesPerTick, MenuMotionEnd::None);
                PlayUiSound(callbacks, screenContext, 8u);
                handled = true;
            }
            else
            {
                OpenOverlayMenu();
                PlayUiSound(callbacks, screenContext, 6u);
                handled = true;
            }
        }

        if (g_replayFlowState.overlayMenuOpen && g_replayFlowState.keysPage)
        {
            if (!handled && !latch && vertical != 0)
            {
                const std::size_t step = (vertical > 0) ? 1u : kRewindKeysPageRows - 1u;
                g_replayFlowState.overlayMenuSelectedIndex =
                    (g_replayFlowState.overlayMenuSelectedIndex + step) % kRewindKeysPageRows;
                g_replayFlowState.notice[0] = '\0';
                PlayUiSound(callbacks, screenContext, 8u);
                handled = true;
            }
            if (!handled && confirm != 0)
            {
                g_replayFlowState.notice[0] = '\0';
                if (g_replayFlowState.overlayMenuSelectedIndex + 1u == kRewindKeysPageRows)
                {
                    replay::settings::RestoreRewindKeyDefaults();
                }
                else
                {
                    std::uint8_t raw[256];
                    (void)ReadRewindKeyboard(raw);
                    bool down[256];
                    for (int k = 0; k < 256; ++k)
                    {
                        down[k] = (raw[k] & 0x80) != 0;
                    }
                    replay::input::BeginKeyCapture(g_replayFlowState.capture, down);
                    g_replayFlowState.captureRow = static_cast<int>(g_replayFlowState.overlayMenuSelectedIndex);
                }
                PlayUiSound(callbacks, screenContext, 6u);
                handled = true;
            }
            if (!handled && cancel != 0)
            {
                LeaveKeysPage();
                g_replayFlowState.overlayMenuSelectedIndex = static_cast<std::size_t>(OptionsItem::RewindKeys);
                PlayUiSound(callbacks, screenContext, 8u);
                handled = true;
            }
        }
        else if (g_replayFlowState.overlayMenuOpen && g_replayFlowState.filterPage)
        {
            using replay::hooks::FilterPageRow;
            constexpr std::size_t kRows = static_cast<std::size_t>(FilterPageRow::Count);
            const auto row = static_cast<FilterPageRow>(g_replayFlowState.overlayMenuSelectedIndex);
            if (!handled && !latch && vertical != 0)
            {
                g_replayFlowState.overlayMenuSelectedIndex =
                    (g_replayFlowState.overlayMenuSelectedIndex + (vertical > 0 ? 1u : kRows - 1u)) % kRows;
                PlayUiSound(callbacks, screenContext, 8u);
                handled = true;
            }
            const int step = (!handled && !latch && horizontal != 0 && row != FilterPageRow::Clear)
                ? horizontal
                : ((!handled && confirm != 0) ? 1 : 0);
            if (step != 0)
            {
                replay::browser::ReplayBrowserFilter filter = g_replayFlowState.browser.GetFilter();
                if (row == FilterPageRow::Sort)
                {
                    (void)StepReplaySortOrder(screenContext, step);
                }
                else if (row == FilterPageRow::Character1 || row == FilterPageRow::Character2)
                {
                    int& character = filter.characters[row == FilterPageRow::Character1 ? 0 : 1];
                    character = replay::name::StepReplayCharacterFilter(character, step);
                    ApplyReplayFilter(screenContext, filter);
                }
                else if (row == FilterPageRow::Player)
                {
                    StepPlayerFilter(screenContext, step);
                }
                else
                {
                    ApplyReplayFilter(screenContext, {});
                }
                PlayUiSound(callbacks, screenContext, 6u);
                handled = true;
            }
            if (!handled && cancel != 0)
            {
                LeaveKeysPage();
                g_replayFlowState.overlayMenuSelectedIndex = static_cast<std::size_t>(OptionsItem::Sort);
                PlayUiSound(callbacks, screenContext, 8u);
                handled = true;
            }
        }
        else if (g_replayFlowState.overlayMenuOpen)
        {
            using replay::hooks::OptionsItem;
            const std::size_t kItemCount = static_cast<std::size_t>(OptionsItem::Count) - (IsRewindAvailable() ? 0u : 1u);
            const auto item = static_cast<OptionsItem>(g_replayFlowState.overlayMenuSelectedIndex);
            if (!handled && !latch && vertical != 0)
            {
                const std::size_t step = (vertical > 0) ? 1u : kItemCount - 1u;
                g_replayFlowState.overlayMenuSelectedIndex = (g_replayFlowState.overlayMenuSelectedIndex + step) % kItemCount;
                PlayUiSound(callbacks, screenContext, 8u);
                handled = true;
            }

            if (!handled && confirm != 0 && item == OptionsItem::Sort)
            {
                g_replayFlowState.filterPage = true;
                g_replayFlowState.overlayMenuSelectedIndex = 0;
                PlayUiSound(callbacks, screenContext, 6u);
                handled = true;
            }

            const int valueStep = (!handled && !latch && horizontal != 0) ? horizontal : ((!handled && confirm != 0) ? 1 : 0);
            if (valueStep != 0 && item == OptionsItem::Sort)
            {
                if (StepReplaySortOrder(screenContext, valueStep))
                {
                    PlayUiSound(callbacks, screenContext, 6u);
                }
                handled = true;
            }
            else if (valueStep != 0 && item == OptionsItem::BackgroundScroll)
            {
                replay::settings::SetBackgroundScroll(!replay::settings::GetBackgroundScroll());
                PlayUiSound(callbacks, screenContext, 6u);
                handled = true;
            }
            else if (valueStep != 0 && item == OptionsItem::Language)
            {
                const int next = (static_cast<int>(replay::settings::GetLanguage()) + (valueStep > 0 ? 1 : 2)) % 3;
                replay::settings::SetLanguage(static_cast<replay::settings::Language>(next));
                PlayUiSound(callbacks, screenContext, 6u);
                mod::Log("ReplayFlow::OverlayMenu: language -> %d", next);
                handled = true;
            }

            if (!handled && confirm != 0)
            {
                if (item == OptionsItem::RecordReplay)
                {
                    handled = BeginRecordReplay(screenContext, callbacks, playerIndex, "options menu");
                }
                else if (item == OptionsItem::PlayFolder)
                {
                    if (StartFolderPlayback(screenContext))
                    {
                        PlayUiSound(callbacks, screenContext, 6u);
                    }
                    handled = true;
                }
                else if (item == OptionsItem::RefreshFolder)
                {
                    if (RefreshCurrentReplayFolder(screenContext))
                    {
                        PlayUiSound(callbacks, screenContext, 6u);
                    }
                    handled = true;
                }
                else if (item == OptionsItem::RewindKeys)
                {
                    g_replayFlowState.keysPage = true;
                    g_replayFlowState.overlayMenuSelectedIndex = 0;
                    PlayUiSound(callbacks, screenContext, 6u);
                    handled = true;
                }
            }

            if (!handled && cancel != 0)
            {
                BeginMenuMotion(-kMenuMotionDegreesPerTick, MenuMotionEnd::None);
                PlayUiSound(callbacks, screenContext, 8u);
                handled = true;
            }
        }

        if (!handled && !g_replayFlowState.overlayMenuOpen && !latch && vertical != 0)
        {
            if (g_replayFlowState.browser.MoveSelection(vertical))
            {
                PlayUiSound(callbacks, screenContext, 8u);
                SyncLegacyReplayGridFromBrowser(screenContext);
                g_replayFlowState.rowNamesDirty = true;
                handled = true;
                mod::LogVerbose(
                    "ReplayFlow::OnSub76F060HandleListInput: vertical move handled player=%d delta=%d",
                    playerIndex,
                    static_cast<int>(vertical));
            }
            else
            {
                mod::Log(
                    "ReplayFlow::OnSub76F060HandleListInput: vertical move no-op player=%d delta=%d",
                    playerIndex,
                    static_cast<int>(vertical));
            }
        }

        if (!handled && !g_replayFlowState.overlayMenuOpen && !latch && horizontal != 0)
        {
            const int pageDelta = (horizontal > 0) ? 1 : -1;
            if (g_replayFlowState.browser.MovePage(pageDelta))
            {
                PlayUiSound(callbacks, screenContext, 8u);
                SyncLegacyReplayGridFromBrowser(screenContext);
                g_replayFlowState.rowNamesDirty = true;
                handled = true;
                mod::LogVerbose(
                    "ReplayFlow::OnSub76F060HandleListInput: horizontal page move handled player=%d delta=%d",
                    playerIndex,
                    pageDelta);
            }
            else
            {
                mod::Log(
                    "ReplayFlow::OnSub76F060HandleListInput: horizontal page move no-op player=%d delta=%d",
                    playerIndex,
                    pageDelta);
            }
        }

        if (!handled && !g_replayFlowState.overlayMenuOpen && record != 0)
        {
            const std::uint32_t sys = GetSystemPtr(screenContext);
            if (sys != 0)
            {
                std::uint8_t recordSlot = 0;
                std::uint8_t recordFilter = 0;
                std::filesystem::path recordPath;
                bool wouldOverwrite = false;
                if (TryAllocateRecordSlot(&recordSlot, &recordFilter, &recordPath, &wouldOverwrite))
                {
                    ByteAt(sys, kSysReplaySlot) = recordSlot;
                    ByteAt(sys, kSysReplayFilter) = recordFilter;
                    ByteAt(sys, kSysReplayIoMode) = 2; // Record mode.
                    ByteAt(screenContext, kOffsetSelectedRow) = recordSlot;
                    ByteAt(screenContext, kOffsetLegacyFilter) = recordFilter;
                    ByteAt(screenContext, kOffsetExitFlag) = 2;
                    g_replayFlowState.selectedReplayPathFs.clear();
                    g_replayFlowState.selectedReplayPath.clear();
                    g_replayFlowState.pendingReplayLaunch = false;
                    g_replayFlowState.pendingRecordAbsPathFs = recordPath;
                    PlayUiSound(callbacks, screenContext, 6u);
                    handled = true;
                    mod::Log(
                        "ReplayFlow::OnSub76F060HandleListInput: record requested via C player=%d slot=%u filter=%u ioMode=%u path='%s' overwrite=%d",
                        playerIndex,
                        static_cast<unsigned>(ByteAt(sys, kSysReplaySlot)),
                        static_cast<unsigned>(ByteAt(sys, kSysReplayFilter)),
                        static_cast<unsigned>(ByteAt(sys, kSysReplayIoMode)),
                        PathForLog(recordPath).c_str(),
                        wouldOverwrite ? 1 : 0);
                }
            }
            else
            {
                mod::Log("ReplayFlow::OnSub76F060HandleListInput: record requested via C but sys ptr was null");
            }
        }

        if (!handled && !g_replayFlowState.overlayMenuOpen && confirm != 0)
        {
            const replay::browser::ReplayBrowserEntry* selected = g_replayFlowState.browser.SelectedEntry();
            mod::Log(
                "ReplayFlow::OnSub76F060HandleListInput: confirm selected=%p",
                static_cast<const void*>(selected));
            if (selected != nullptr && selected->kind == replay::browser::BrowserEntryKind::Directory)
            {
                PlayUiSound(callbacks, screenContext, 6u);
                BeginMenuMotion(kMenuMotionDegreesPerTick, MenuMotionEnd::EnterFolder);
                handled = true;
            }
            else if (selected != nullptr && selected->kind == replay::browser::BrowserEntryKind::ReplayFile && selected->playable)
            {
                replay::hooks::StopFolderPlayback();
                g_replayFlowState.pendingReplayLaunch = false;
                const std::uint32_t sys = GetSystemPtr(screenContext);
                if (sys != 0 && PrepareReplayPath(selected->absolutePathFs, sys))
                {
                    const replay::browser::ReplayBrowserView* curView = g_replayFlowState.browser.CurrentView();
                    g_replayFlowState.restoreRelativeFolder = (curView != nullptr) ? curView->relativeFolderPath : std::string();
                    PlayUiSound(callbacks, screenContext, 6u);
                    ByteAt(screenContext, kOffsetLegacyFilter) = 0;
                    g_replayFlowState.pendingReplayLaunch = true;
                    mod::Log(
                        "ReplayFlow::OnSub76F060HandleListInput: confirm replay selected path='%s' sysReplay(slot=%u filter=%u mode=%u) pendingLaunch=1",
                        g_replayFlowState.selectedReplayPath.c_str(),
                        static_cast<unsigned>(ByteAt(sys, kSysReplaySlot)),
                        static_cast<unsigned>(ByteAt(sys, kSysReplayFilter)),
                        static_cast<unsigned>(ByteAt(sys, kSysReplayIoMode)));
                }
                else
                {
                    mod::Log(
                        "ReplayFlow::OnSub76F060HandleListInput: confirm replay not launched (sys=0x%08X)",
                        static_cast<unsigned>(sys));
                }
                handled = true;
            }
            else if (selected == nullptr)
            {
                const replay::browser::ReplayBrowserView* emptyView = g_replayFlowState.browser.CurrentView();
                if (g_replayFlowState.browser.GetFilter().Active() && emptyView != nullptr && emptyView->totalReplays > 0)
                {
                    ApplyReplayFilter(screenContext, {});
                    PlayUiSound(callbacks, screenContext, 6u);
                }
                else if (g_replayFlowState.browser.IsInSubfolder())
                {
                    PlayUiSound(callbacks, screenContext, 8u);
                    BeginMenuMotion(kMenuMotionDegreesPerTick, MenuMotionEnd::LeaveFolder);
                }
                else
                {
                    ByteAt(screenContext, kOffsetExitFlag) = 1;
                }
                handled = true;
            }
            else
            {
                mod::Log("ReplayFlow::OnSub76F060HandleListInput: confirm ignored (non-playable entry)");
            }
        }

        if (!handled && !g_replayFlowState.overlayMenuOpen && cancel != 0 && ByteAt(screenContext, kOffsetMenuState) == 0)
        {
            if (g_replayFlowState.browser.IsInSubfolder())
            {
                PlayUiSound(callbacks, screenContext, 8u);
                BeginMenuMotion(kMenuMotionDegreesPerTick, MenuMotionEnd::LeaveFolder);
                handled = true;
            }
            else
            {
                ByteAt(screenContext, kOffsetExitFlag) = 1;
                handled = true;
                mod::Log("ReplayFlow::OnSub76F060HandleListInput: cancel at root -> set exit flag");
            }
        }

        if (handled || logThisFrame)
        {
            mod::LogVerbose(
                "ReplayFlow::OnSub76F060HandleListInput: end player=%d handled=%d menuState=%u selectedRow=%u",
                playerIndex,
                handled ? 1 : 0,
                static_cast<unsigned>(ByteAt(screenContext, kOffsetMenuState)),
                static_cast<unsigned>(ByteAt(screenContext, kOffsetSelectedRow)));
        }
        return handled;
    }
    catch (const std::exception& ex)
    {
        mod::Log(
            "ReplayFlow::OnSub76F060HandleListInput: exception player=%d what='%s'",
            playerIndex,
            ex.what());
        return false;
    }
    catch (...)
    {
        mod::Log(
            "ReplayFlow::OnSub76F060HandleListInput: unknown exception player=%d",
            playerIndex);
        return false;
    }
}

void UpdateReplayInputLatch(std::uint32_t screenContext, int playerIndex)
{
    if (playerIndex < 0 || playerIndex > 1)
    {
        mod::Log("ReplayFlow::UpdateReplayInputLatch: skip invalid player index=%d", playerIndex);
        return;
    }

    const bool active = (ReadInputAxis(screenContext, playerIndex, kSysInputHorizontal) != 0)
        || (ReadInputAxis(screenContext, playerIndex, kSysInputVertical) != 0)
        || (ReadInputButton(screenContext, playerIndex, kSysInputCancel) != 0)
        || (ReadInputButton(screenContext, playerIndex, kSysInputRecord) != 0)
        || (ReadInputButton(screenContext, playerIndex, kSysInputMenu) != 0);
    ByteAt(screenContext, kOffsetInputLatchBase + static_cast<std::uint32_t>(playerIndex)) = active ? 1 : 0;
    const int activeInt = active ? 1 : 0;
    if (g_lastLatchActive[playerIndex] != activeInt || ShouldLogEvery(&g_lastLatchHeartbeatTick[playerIndex], 4000))
    {
        mod::LogVerbose(
            "ReplayFlow::UpdateReplayInputLatch: screen=0x%08X player=%d active=%d prev=%d",
            static_cast<unsigned>(screenContext),
            playerIndex,
            activeInt,
            g_lastLatchActive[playerIndex]);
    }
    g_lastLatchActive[playerIndex] = activeInt;
}

bool GetVisibleReplayRows(
    std::vector<browser::ReplayBrowserEntry>* outRows,
    std::size_t* outSelectedVisibleRow,
    std::string* outRelativeFolder)
{
    if (outRows == nullptr || outSelectedVisibleRow == nullptr || outRelativeFolder == nullptr || !g_replayFlowState.browserReady)
    {
        if (ShouldLogEvery(&g_lastRowsFailedArgsTick, 2000))
        {
            mod::Log(
                "ReplayFlow::GetVisibleReplayRows: failed args rows=%d selected=%d folder=%d browserReady=%d",
                (outRows != nullptr) ? 1 : 0,
                (outSelectedVisibleRow != nullptr) ? 1 : 0,
                (outRelativeFolder != nullptr) ? 1 : 0,
                g_replayFlowState.browserReady ? 1 : 0);
        }
        return false;
    }

    outRows->clear();
    *outSelectedVisibleRow = 0;
    outRelativeFolder->clear();

    g_replayFlowState.browser.EnsureVisibleReplayMetadataLoaded();
    const browser::ReplayBrowserView* view = g_replayFlowState.browser.CurrentView();
    if (view == nullptr)
    {
        mod::Log("ReplayFlow::GetVisibleReplayRows: failed (current view null)");
        return false;
    }

    const std::size_t start = g_replayFlowState.browser.VisibleStartIndex();
    const std::size_t end = g_replayFlowState.browser.VisibleEndIndexExclusive();
    outRows->reserve((end > start) ? (end - start) : 0);
    for (std::size_t i = start; i < end; ++i)
    {
        outRows->push_back(view->entries[i]);
    }

    *outSelectedVisibleRow = g_replayFlowState.browser.SelectedVisibleRow();
    *outRelativeFolder = view->relativeFolderPath;
    const bool changed = (g_lastRowsFolder != *outRelativeFolder)
        || (g_lastRowsStart != start)
        || (g_lastRowsEnd != end)
        || (g_lastRowsCount != outRows->size())
        || (g_lastRowsSelectedVisible != *outSelectedVisibleRow);
    if (changed || ShouldLogEvery(&g_lastRowsSnapshotTick, 2500))
    {
        mod::LogVerbose(
            "ReplayFlow::GetVisibleReplayRows: folder='%s' start=%u end=%u rows=%u selectedVisibleRow=%u changed=%d",
            outRelativeFolder->c_str(),
            static_cast<unsigned>(start),
            static_cast<unsigned>(end),
            static_cast<unsigned>(outRows->size()),
            static_cast<unsigned>(*outSelectedVisibleRow),
            changed ? 1 : 0);
    }
    g_lastRowsFolder = *outRelativeFolder;
    g_lastRowsStart = start;
    g_lastRowsEnd = end;
    g_lastRowsCount = outRows->size();
    g_lastRowsSelectedVisible = *outSelectedVisibleRow;
    return true;
}

bool ConsumeRowNamesDirtyFlag()
{
    const bool value = g_replayFlowState.rowNamesDirty;
    g_replayFlowState.rowNamesDirty = false;
    if (value)
    {
        mod::LogVerbose("ReplayFlow::ConsumeRowNamesDirtyFlag: consumed=1");
    }
    return value;
}

bool TickMenuMotion(std::uint32_t screenContext)
{
    if (g_replayFlowState.motionStep == 0.0)
    {
        return false;
    }

    // D held through the motion is not a new press when it ends.
    const std::uint32_t sys = GetSystemPtr(screenContext);
    for (std::uint32_t player = 0; sys != 0 && player < 2; ++player)
    {
        g_menuHeldLast[player] = *reinterpret_cast<const std::uint32_t*>(sys + kSysInputMenuHeld + 4u * player) != 0;
    }

    g_replayFlowState.motionAngle += g_replayFlowState.motionStep;
    if (g_replayFlowState.motionAngle >= 180.0)
    {
        g_replayFlowState.motionAngle = 180.0;
        const MenuMotionEnd end = g_replayFlowState.motionEnd;
        BeginMenuMotion(0.0, MenuMotionEnd::None);
        if (end != MenuMotionEnd::None)
        {
            try
            {
                (void)(end == MenuMotionEnd::EnterFolder ? g_replayFlowState.browser.EnterSelectedDirectory()
                                                         : g_replayFlowState.browser.ReturnToParentDirectory());
            }
            catch (const std::exception& ex)
            {
                mod::Log("ReplayFlow::TickMenuMotion: folder change failed what='%s'", ex.what());
            }
            SyncLegacyReplayGridFromBrowser(screenContext);
            g_replayFlowState.rowNamesDirty = true;
            BeginMenuMotion(-kMenuMotionDegreesPerTick, MenuMotionEnd::None);
        }
    }
    else if (g_replayFlowState.motionAngle <= 0.0)
    {
        g_replayFlowState.motionAngle = 0.0;
        BeginMenuMotion(0.0, MenuMotionEnd::None);
        CloseOverlayMenu("motion end");
    }
    return true;
}

double GetMenuMotionAngle()
{
    return g_replayFlowState.motionAngle;
}

void TickRewindKeyCapture(std::uint32_t screenContext, const ReplayFlowCallbacks& callbacks)
{
    if (g_replayFlowState.captureRow < 0 && !g_replayFlowState.escGuard)
    {
        return;
    }
    constexpr int kDikEscape = 1;
    std::uint8_t raw[256];
    const bool focused = ReadRewindKeyboard(raw);
    if (g_replayFlowState.escGuard && (raw[kDikEscape] & 0x80) == 0)
    {
        g_replayFlowState.escGuard = false;
    }
    if (g_replayFlowState.captureRow < 0)
    {
        return;
    }
    replay::input::KeyCapture& capture = g_replayFlowState.capture;
    if (!focused)
    {
        // Alt-tab: nothing typed elsewhere may bind, and everything must be released again.
        std::memset(capture.prev, 0, sizeof(capture.prev));
        capture.primed = false;
        return;
    }
    bool down[256];
    for (int k = 0; k < 256; ++k)
    {
        down[k] = (raw[k] & 0x80) != 0;
    }
    const bool esc = down[kDikEscape] && !capture.prev[kDikEscape];
    const std::uint32_t sys = GetSystemPtr(screenContext);
    const bool gameCancel = sys != 0 && (ByteAt(sys, kSysInputCancel) != 0 || ByteAt(sys, kSysInputCancel + 1u) != 0);
    if (esc || gameCancel)
    {
        g_replayFlowState.escGuard = esc;
        g_replayFlowState.captureRow = -1;
        capture.active = false;
        PlayUiSound(callbacks, screenContext, 8u);
        return;
    }
    if (down[0x38] || down[0xB8] || down[0xDB] || down[0xDC])
    {
        // Alt+Enter, Alt+Tab, Win+anything belong to the OS: everything must be released again.
        std::memcpy(capture.prev, down, sizeof(capture.prev));
        capture.primed = false;
        return;
    }
    const int dik = replay::input::StepKeyCapture(capture, down, &replay::settings::IsExcludedKey);
    if (dik == 0)
    {
        return;
    }
    const auto action = static_cast<replay::settings::RewindKey>(g_replayFlowState.captureRow);
    if (replay::settings::IsRewindKeyUsedByOther(action, dik))
    {
        std::snprintf(g_replayFlowState.notice, sizeof(g_replayFlowState.notice),
            replay::i18n::Tr(replay::i18n::Text::KeyInUseFmt), replay::i18n::KeyName(dik).c_str());
        PlayUiSound(callbacks, screenContext, 8u);
        return;
    }
    replay::settings::SetRewindKey(action, dik);
    g_replayFlowState.notice[0] = '\0';
    g_replayFlowState.captureRow = -1;
    capture.active = false;
    PlayUiSound(callbacks, screenContext, 6u);
}

bool IsKeyCaptureBusy()
{
    return g_replayFlowState.captureRow >= 0 || g_replayFlowState.escGuard;
}

bool GetOverlayMenuState(ReplayOverlayMenuState* outState)
{
    if (outState == nullptr)
    {
        return false;
    }

    *outState = {};
    outState->open = g_replayFlowState.overlayMenuOpen;
    outState->selectedIndex = g_replayFlowState.overlayMenuSelectedIndex;
    outState->rewindAvailable = IsRewindAvailable();
    outState->keysPage = g_replayFlowState.keysPage;
    outState->filterPage = g_replayFlowState.filterPage;
    outState->captureRow = g_replayFlowState.captureRow;
    outState->capturePrimed = g_replayFlowState.capture.primed;
    std::snprintf(outState->notice, sizeof(outState->notice), "%s", g_replayFlowState.notice);
    if (g_replayFlowState.browserReady)
    {
        outState->sortOrder = g_replayFlowState.browser.GetReplaySortOrder();
        const replay::browser::ReplayBrowserFilter& filter = g_replayFlowState.browser.GetFilter();
        outState->filterActive = filter.Active();
        outState->filterCharacters[0] = filter.characters[0];
        outState->filterCharacters[1] = filter.characters[1];
        std::snprintf(outState->filterPlayer, sizeof(outState->filterPlayer), "%s", filter.player.c_str());
        if (const replay::browser::ReplayBrowserView* view = g_replayFlowState.browser.CurrentView())
        {
            outState->shownReplays = static_cast<unsigned>(view->shownReplays);
            outState->totalReplays = static_cast<unsigned>(view->totalReplays);
        }
    }
    else
    {
        outState->sortOrder = replay::settings::GetSortOrder();
    }
    return true;
}

bool GetSelectedReplayPath(std::string* outPath)
{
    if (outPath == nullptr || g_replayFlowState.selectedReplayPathFs.empty())
    {
        return false;
    }
    *outPath = g_replayFlowState.selectedReplayPath;
    if (outPath->empty())
    {
        *outPath = PathForLog(g_replayFlowState.selectedReplayPathFs);
    }
    mod::Log("ReplayFlow::GetSelectedReplayPath: path='%s'", outPath->c_str());
    return true;
}

bool GetSelectedReplayPathFs(std::filesystem::path* outPath)
{
    if (outPath == nullptr || g_replayFlowState.selectedReplayPathFs.empty())
    {
        return false;
    }
    *outPath = g_replayFlowState.selectedReplayPathFs;
    return true;
}

void ClearSelectedReplayPath()
{
    mod::Log(
        "ReplayFlow::ClearSelectedReplayPath: hadPath=%d",
        g_replayFlowState.selectedReplayPathFs.empty() ? 0 : 1);
    g_replayFlowState.selectedReplayPathFs.clear();
    g_replayFlowState.selectedReplayPath.clear();
}

bool ConsumePendingReplayLaunch()
{
    const bool value = g_replayFlowState.pendingReplayLaunch;
    g_replayFlowState.pendingReplayLaunch = false;
    if (value)
    {
        mod::Log("ReplayFlow::ConsumePendingReplayLaunch: consumed=1");
    }
    return value;
}
}
