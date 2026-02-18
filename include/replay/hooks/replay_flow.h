#pragma once

#include "replay/core/replay_browser.h"
#include "replay/core/replay_file_metadata.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace replay::hooks
{
struct ReplayFlowCallbacks
{
    void (*playUiSound)(std::uint32_t screenContext, unsigned short soundIndex) = nullptr;
};

enum class OptionsItem : std::size_t
{
    RecordReplay = 0,
    Sort,
    PlayFolder,
    RefreshFolder,
    BackgroundScroll,
    Language,
    RewindKeys,
    Count,
};

constexpr std::size_t kRewindKeysPageRows = 5;

enum class FilterPageRow : std::size_t
{
    Sort = 0,
    Character1,
    Character2,
    Player,
    Clear,
    Count,
};

struct ReplayOverlayMenuState
{
    bool open = false;
    std::size_t selectedIndex = 0;
    browser::ReplaySortOrder sortOrder = browser::ReplaySortOrder::NewestFirst;
    bool rewindAvailable = false;
    bool keysPage = false;
    bool filterPage = false;
    bool filterActive = false;
    int filterCharacters[2] = {-1, -1};
    char filterPlayer[64] = {};
    unsigned shownReplays = 0;
    unsigned totalReplays = 0;
    int captureRow = -1;
    bool capturePrimed = false;
    char notice[64] = {};
};

bool OnSub770E30InitializeReplayBrowser(std::uint32_t screenContext, const std::string& replayRootDirectory);

bool OnSub76F060HandleListInput(std::uint32_t screenContext, int playerIndex, const ReplayFlowCallbacks& callbacks);

bool TickMenuMotion(std::uint32_t screenContext);
double GetMenuMotionAngle();

void TickRewindKeyCapture(std::uint32_t screenContext, const ReplayFlowCallbacks& callbacks);
// True while a key capture owns the keyboard, including its Esc release: the vanilla
// Esc exit check and list input must not run.
bool IsKeyCaptureBusy();

void UpdateReplayInputLatch(std::uint32_t screenContext, int playerIndex);

bool GetVisibleReplayRows(
    std::vector<browser::ReplayBrowserEntry>* outRows,
    std::size_t* outSelectedVisibleRow,
    std::string* outRelativeFolder);
bool ConsumeRowNamesDirtyFlag();
bool GetOverlayMenuState(ReplayOverlayMenuState* outState);

bool GetSelectedReplayPath(std::string* outPath);
bool GetSelectedReplayPathFs(std::filesystem::path* outPath);
void ClearSelectedReplayPath();
bool ConsumePendingReplayLaunch();

// True once the openFileReader + CreateFile path-swap hooks are in place (installs
// the CreateFile pair on first call); launches are refused otherwise.
bool IsReplayRedirectReady();

file::ReplayLength GetLaunchedReplayLength();

bool IsFolderPlaybackActive();
void StopFolderPlayback();
bool ContinueFolderPlayback(std::uint32_t systemContext);
// Battle update filter: after a finished playback (8 = replay menu) returns 2 (loading)
// with the next replay armed; a user cancel stops the queue.
char OnReplayBattleUpdateResult(std::uint32_t systemContext, char nextScreen, bool wasPlayback, bool cancelled);

bool RefreshCurrentReplayFolder(std::uint32_t screenContext);
}
