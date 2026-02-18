#pragma once

#include "replay/core/replay_file_metadata.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace replay::browser
{
enum class BrowserEntryKind
{
    Directory = 0,
    ReplayFile = 1,
};

enum class ReplaySortOrder
{
    OldestFirst = 0,
    NewestFirst = 1,
    PlayerName = 2,
    LargestFirst = 3,
};

struct ReplayBrowserFilter
{
    int characters[2] = {-1, -1}; // ids as replay headers store them, -1 = all
    std::string player;

    bool HasCharacter() const { return characters[0] >= 0 || characters[1] >= 0; }
    bool Active() const { return HasCharacter() || !player.empty(); }
};

struct ReplayBrowserConfig
{
    std::size_t visibleRows = 9;
    bool prettifyConcreteNames = true;
    bool ignoreUnusableReplayFiles = true;
    bool sortDirectoriesFirst = true;
    ReplaySortOrder replaySortOrder = ReplaySortOrder::OldestFirst;
};

struct ReplayBrowserEntry
{
    BrowserEntryKind kind = BrowserEntryKind::ReplayFile;
    bool playable = false;
    bool concreteNameMatched = false;
    bool metadataLoaded = false;

    std::string displayName;
    std::string sortKey;
    std::filesystem::path absolutePathFs;
    std::string relativePath;

    std::uint64_t dateKey = 0; // YYYYMMDDHHMMSS local time, from the file name, else the file's modified time
    std::uintmax_t sizeBytes = 0;
    std::string players[2];
    std::string playerKey;
    int characters[2] = {-1, -1}; // from the file name; -1 = read the header if a filter needs it, -2 = unreadable

    file::ReplayHeaderMetadata metadata = {};
};

struct ReplayBrowserView
{
    std::string relativeFolderPath;
    std::vector<ReplayBrowserEntry> entries;
    std::vector<ReplayBrowserEntry> all;
    std::size_t totalReplays = 0;
    std::size_t shownReplays = 0;
    std::size_t selectedIndex = 0;
    std::size_t firstVisibleIndex = 0;
};

class ReplayBrowser
{
public:
    ReplayBrowser() = default;

    bool Initialize(const std::string& rootReplayDirectory, const ReplayBrowserConfig& config);
    bool RefreshCurrentFolder();

    bool MoveSelection(int delta);
    bool MovePage(int deltaPages);
    bool EnterSelectedDirectory();
    bool ReturnToParentDirectory();
    bool IsInSubfolder() const { return folderStack_.size() > 1; }

    const ReplayBrowserView* CurrentView() const;
    const ReplayBrowserEntry* SelectedEntry() const;
    bool HasEntries() const;
    std::size_t VisibleRows() const;
    void EnsureVisibleReplayMetadataLoaded();

    std::size_t VisibleStartIndex() const;
    std::size_t VisibleEndIndexExclusive() const;
    std::size_t SelectedVisibleRow() const;

    bool SetReplaySortOrder(ReplaySortOrder order);
    ReplaySortOrder GetReplaySortOrder() const;
    void SetFilter(const ReplayBrowserFilter& filter);
    const ReplayBrowserFilter& GetFilter() const;
    std::vector<std::string> PlayerOptions() const;
    const std::filesystem::path& GetRootAbsolutePathFs() const;
    const std::string& GetRootAbsolutePath() const;

    bool NavigateToAndSelect(const std::string& relativeFolder, const std::filesystem::path& targetAbsPath);

private:
    struct FolderFrame
    {
        ReplayBrowserView view = {};
    };

    bool ScanFolder(const std::string& relativeFolderPath, ReplayBrowserView* outView) const;
    void SortEntries(ReplayBrowserView* view) const;
    void ApplyFilter(ReplayBrowserView* view) const;
    void ResortKeepingSelection(ReplayBrowserView* view) const;
    void ClampSelection(ReplayBrowserView* view) const;
    void EnsureSelectionVisible(ReplayBrowserView* view) const;

    std::string rootAbsolutePath_;
    std::filesystem::path rootAbsolutePathFs_;
    ReplayBrowserConfig config_ = {};
    ReplayBrowserFilter filter_ = {};
    std::vector<FolderFrame> folderStack_;
};
}
