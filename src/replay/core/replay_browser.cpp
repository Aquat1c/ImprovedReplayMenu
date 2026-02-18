#include "replay/core/replay_browser.h"

#include "logger.h"
#include "replay/core/replay_name_format.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <system_error>
#include <utility>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
std::string LowerAscii(std::string v)
{
    for (char& c : v)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return v;
}

std::string WideToMultiByte(UINT codePage, const std::wstring& wide, bool* outUsedDefaultChar)
{
#if defined(_WIN32)
    if (outUsedDefaultChar != nullptr)
    {
        *outUsedDefaultChar = false;
    }
    if (wide.empty())
    {
        return std::string();
    }

    BOOL usedDefault = FALSE;
    int required = WideCharToMultiByte(
        codePage,
        0,
        wide.c_str(),
        -1,
        nullptr,
        0,
        (codePage == CP_UTF8) ? nullptr : "?",
        (codePage == CP_UTF8) ? nullptr : &usedDefault);
    if (required <= 0)
    {
        return std::string();
    }

    std::string out(static_cast<std::size_t>(required), '\0');
    const int written = WideCharToMultiByte(
        codePage,
        0,
        wide.c_str(),
        -1,
        out.data(),
        required,
        (codePage == CP_UTF8) ? nullptr : "?",
        (codePage == CP_UTF8) ? nullptr : &usedDefault);
    if (written <= 0)
    {
        return std::string();
    }
    if (!out.empty() && out.back() == '\0')
    {
        out.pop_back();
    }
    if (outUsedDefaultChar != nullptr)
    {
        *outUsedDefaultChar = (usedDefault != FALSE);
    }
    return out;
#else
    (void)codePage;
    if (outUsedDefaultChar != nullptr)
    {
        *outUsedDefaultChar = false;
    }
    return std::string();
#endif
}

std::string NarrowPathBestEffort(const std::filesystem::path& p)
{
#if defined(_WIN32)
    return WideToMultiByte(CP_ACP, p.wstring(), nullptr);
#else
    return p.string();
#endif
}

std::string Utf8PathBestEffort(const std::filesystem::path& p)
{
#if defined(_WIN32)
    return WideToMultiByte(CP_UTF8, p.wstring(), nullptr);
#else
    return p.string();
#endif
}

std::uint64_t LocalDateKey(std::filesystem::file_time_type time)
{
    const auto ticks = static_cast<std::uint64_t>(time.time_since_epoch().count());
    FILETIME utc = {static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    FILETIME local = {};
    SYSTEMTIME st = {};
    if (ticks == 0 || !FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st))
    {
        return 0;
    }
    return st.wYear * 10000000000ull + st.wMonth * 100000000ull + st.wDay * 1000000ull + st.wHour * 10000ull
        + st.wMinute * 100ull + st.wSecond;
}

std::string PathForLog(const std::filesystem::path& p)
{
    const std::string path = NarrowPathBestEffort(p);
    return path.empty() ? std::string("<unrepresentable-path>") : path;
}

// Browser strings are UTF-8; constructing a path from a plain std::string reads it as ANSI.
std::filesystem::path Utf8ToPath(const std::string& utf8)
{
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string ToGenericPathString(const std::filesystem::path& p)
{
    std::string path = Utf8PathBestEffort(p);
    for (char& c : path)
    {
        if (c == '\\')
        {
            c = '/';
        }
    }
    return path;
}
}

namespace replay::browser
{
void ReplayBrowser::SortEntries(ReplayBrowserView* view) const
{
    if (view == nullptr)
    {
        return;
    }

    std::vector<ReplayBrowserEntry>& source = view->all.empty() ? view->entries : view->all;
    std::stable_sort(
        source.begin(),
        source.end(),
        [this](const ReplayBrowserEntry& a, const ReplayBrowserEntry& b) {
            if (config_.sortDirectoriesFirst && a.kind != b.kind)
            {
                return a.kind == BrowserEntryKind::Directory;
            }
            if (a.kind == BrowserEntryKind::ReplayFile && b.kind == BrowserEntryKind::ReplayFile)
            {
                switch (config_.replaySortOrder)
                {
                case ReplaySortOrder::NewestFirst:
                case ReplaySortOrder::OldestFirst:
                    if (a.dateKey != b.dateKey)
                    {
                        if (a.dateKey == 0 || b.dateKey == 0)
                        {
                            return b.dateKey == 0;
                        }
                        return (config_.replaySortOrder == ReplaySortOrder::NewestFirst) == (a.dateKey > b.dateKey);
                    }
                    break;
                case ReplaySortOrder::PlayerName:
                    if (a.playerKey != b.playerKey)
                    {
                        if (a.playerKey.empty() || b.playerKey.empty())
                        {
                            return b.playerKey.empty();
                        }
                        return a.playerKey < b.playerKey;
                    }
                    if (a.dateKey != b.dateKey)
                    {
                        return a.dateKey > b.dateKey;
                    }
                    break;
                case ReplaySortOrder::LargestFirst:
                    if (a.sizeBytes != b.sizeBytes)
                    {
                        return a.sizeBytes > b.sizeBytes;
                    }
                    break;
                }
            }
            return a.sortKey < b.sortKey;
        });
    ApplyFilter(view);
}

void ReplayBrowser::ApplyFilter(ReplayBrowserView* view) const
{
    const auto countReplays = [](const std::vector<ReplayBrowserEntry>& list) {
        return static_cast<std::size_t>(std::count_if(list.begin(), list.end(),
            [](const ReplayBrowserEntry& e) { return e.kind == BrowserEntryKind::ReplayFile; }));
    };
    if (!filter_.Active())
    {
        if (!view->all.empty())
        {
            view->entries = std::move(view->all);
            view->all.clear();
        }
        view->totalReplays = view->shownReplays = countReplays(view->entries);
        return;
    }
    if (view->all.empty())
    {
        view->all = std::move(view->entries);
    }
    view->entries.clear();
    view->totalReplays = countReplays(view->all);
    for (ReplayBrowserEntry& e : view->all)
    {
        if (e.kind == BrowserEntryKind::ReplayFile)
        {
            if (filter_.HasCharacter() && e.characters[0] == -1)
            {
                file::ReplayHeaderMetadata header = {};
                const bool parsed = file::ParseReplayHeaderMetadata(e.absolutePathFs, &header);
                e.characters[0] = parsed ? header.p1CharacterId : -2;
                e.characters[1] = parsed ? header.p2CharacterId : -2;
            }
            const int a = filter_.characters[0];
            const int b = filter_.characters[1];
            const bool characterMatch = (a >= 0 && b >= 0)
                ? (e.characters[0] == a && e.characters[1] == b) || (e.characters[0] == b && e.characters[1] == a)
                : !filter_.HasCharacter() || e.characters[0] == (a >= 0 ? a : b) || e.characters[1] == (a >= 0 ? a : b);
            if (!characterMatch
                || (!filter_.player.empty() && e.players[0] != filter_.player && e.players[1] != filter_.player))
            {
                continue;
            }
        }
        view->entries.push_back(e);
    }
    view->shownReplays = countReplays(view->entries);
}

void ReplayBrowser::ResortKeepingSelection(ReplayBrowserView* view) const
{
    std::filesystem::path selectedPath;
    if (view->selectedIndex < view->entries.size())
    {
        selectedPath = view->entries[view->selectedIndex].absolutePathFs;
    }
    SortEntries(view);
    view->selectedIndex = 0;
    view->firstVisibleIndex = 0;
    for (std::size_t i = 0; i < view->entries.size() && !selectedPath.empty(); ++i)
    {
        if (view->entries[i].absolutePathFs == selectedPath)
        {
            view->selectedIndex = i;
            break;
        }
    }
    ClampSelection(view);
    EnsureSelectionVisible(view);
}

void ReplayBrowser::SetFilter(const ReplayBrowserFilter& filter)
{
    filter_ = filter;
    for (FolderFrame& frame : folderStack_)
    {
        ResortKeepingSelection(&frame.view);
    }
}

const ReplayBrowserFilter& ReplayBrowser::GetFilter() const
{
    return filter_;
}

std::vector<std::string> ReplayBrowser::PlayerOptions() const
{
    std::vector<std::string> players;
    if (const ReplayBrowserView* view = CurrentView())
    {
        for (const ReplayBrowserEntry& e : view->all.empty() ? view->entries : view->all)
        {
            for (const std::string& player : e.players)
            {
                if (!player.empty())
                {
                    players.push_back(player);
                }
            }
        }
    }
    std::sort(players.begin(), players.end(), [](const std::string& a, const std::string& b) {
        const std::string left = LowerAscii(a);
        const std::string right = LowerAscii(b);
        return left != right ? left < right : a < b;
    });
    players.erase(std::unique(players.begin(), players.end()), players.end());
    return players;
}

bool ReplayBrowser::Initialize(const std::string& rootReplayDirectory, const ReplayBrowserConfig& config)
{
    config_ = config;
    if (config_.visibleRows == 0)
    {
        config_.visibleRows = 9;
    }

    std::error_code ec;
    const std::filesystem::path rootPath = std::filesystem::weakly_canonical(Utf8ToPath(rootReplayDirectory), ec);
    if (ec || rootPath.empty())
    {
        mod::Log(
            "ReplayBrowser::Initialize: failed canonical root='%s' ec=%d",
            rootReplayDirectory.c_str(),
            ec.value());
        return false;
    }
    if (!std::filesystem::exists(rootPath, ec) || ec)
    {
        mod::Log(
            "ReplayBrowser::Initialize: root does not exist root='%s' ec=%d",
            PathForLog(rootPath).c_str(),
            ec.value());
        return false;
    }
    if (!std::filesystem::is_directory(rootPath, ec) || ec)
    {
        mod::Log(
            "ReplayBrowser::Initialize: root is not directory root='%s' ec=%d",
            PathForLog(rootPath).c_str(),
            ec.value());
        return false;
    }

    rootAbsolutePathFs_ = rootPath;
    rootAbsolutePath_ = PathForLog(rootPath);
    folderStack_.clear();

    ReplayBrowserView rootView = {};
    if (!ScanFolder("", &rootView))
    {
        return false;
    }

    FolderFrame frame = {};
    frame.view = std::move(rootView);
    folderStack_.push_back(std::move(frame));
    EnsureVisibleReplayMetadataLoaded();

    mod::Log(
        "ReplayBrowser::Initialize: ok root='%s' entries=%zu visibleRows=%zu",
        rootAbsolutePath_.c_str(),
        folderStack_.back().view.entries.size(),
        config_.visibleRows);
    return true;
}

bool ReplayBrowser::RefreshCurrentFolder()
{
    if (folderStack_.empty())
    {
        mod::Log("ReplayBrowser::RefreshCurrentFolder: empty stack");
        return false;
    }

    ReplayBrowserView refreshed = {};
    const std::string relative = folderStack_.back().view.relativeFolderPath;
    if (!ScanFolder(relative, &refreshed))
    {
        mod::Log("ReplayBrowser::RefreshCurrentFolder: scan failed relative='%s'", relative.c_str());
        return false;
    }

    refreshed.selectedIndex = folderStack_.back().view.selectedIndex;
    refreshed.firstVisibleIndex = folderStack_.back().view.firstVisibleIndex;
    ClampSelection(&refreshed);
    EnsureSelectionVisible(&refreshed);

    folderStack_.back().view = std::move(refreshed);
    EnsureVisibleReplayMetadataLoaded();
    mod::Log(
        "ReplayBrowser::RefreshCurrentFolder: ok relative='%s' entries=%zu sel=%zu first=%zu",
        folderStack_.back().view.relativeFolderPath.c_str(),
        folderStack_.back().view.entries.size(),
        folderStack_.back().view.selectedIndex,
        folderStack_.back().view.firstVisibleIndex);
    return true;
}

bool ReplayBrowser::MoveSelection(int delta)
{
    if (delta == 0 || folderStack_.empty())
    {
        return false;
    }

    ReplayBrowserView& view = folderStack_.back().view;
    if (view.entries.empty())
    {
        view.selectedIndex = 0;
        view.firstVisibleIndex = 0;
        return false;
    }

    const std::size_t oldSelected = view.selectedIndex;
    const long long count = static_cast<long long>(view.entries.size());
    long long next = static_cast<long long>(view.selectedIndex) + static_cast<long long>(delta);
    next %= count;
    if (next < 0)
    {
        next += count;
    }
    view.selectedIndex = static_cast<std::size_t>(next);

    EnsureSelectionVisible(&view);
    const bool changed = (view.selectedIndex != oldSelected);
    if (changed)
    {
        EnsureVisibleReplayMetadataLoaded();
        mod::LogVerbose(
            "ReplayBrowser::MoveSelection: relative='%s' old=%zu new=%zu first=%zu rows=%zu",
            view.relativeFolderPath.c_str(),
            oldSelected,
            view.selectedIndex,
            view.firstVisibleIndex,
            config_.visibleRows);
    }
    return changed;
}

bool ReplayBrowser::MovePage(int deltaPages)
{
    if (deltaPages == 0 || folderStack_.empty())
    {
        return false;
    }

    ReplayBrowserView& view = folderStack_.back().view;
    if (view.entries.empty())
    {
        view.selectedIndex = 0;
        view.firstVisibleIndex = 0;
        return false;
    }

    const std::size_t oldSelected = view.selectedIndex;
    const std::size_t oldFirst = view.firstVisibleIndex;
    const std::size_t pageRows = (config_.visibleRows > 0) ? config_.visibleRows : 1;
    const std::size_t count = view.entries.size();
    const std::size_t totalPages = (count + pageRows - 1) / pageRows;
    if (totalPages <= 1)
    {
        return false;
    }

    const std::size_t currentPage = view.selectedIndex / pageRows;
    const std::size_t currentPageStart = currentPage * pageRows;
    const std::size_t rowInPage = view.selectedIndex - currentPageStart;

    long long targetPage = static_cast<long long>(currentPage) + static_cast<long long>(deltaPages);
    targetPage %= static_cast<long long>(totalPages);
    if (targetPage < 0)
    {
        targetPage += static_cast<long long>(totalPages);
    }

    const std::size_t targetPageStart = static_cast<std::size_t>(targetPage) * pageRows;
    const std::size_t targetPageEndExclusive = std::min(targetPageStart + pageRows, count);
    const std::size_t targetPageCount = targetPageEndExclusive - targetPageStart;
    const std::size_t clampedRowInPage = std::min(rowInPage, targetPageCount - 1);

    view.firstVisibleIndex = targetPageStart;
    view.selectedIndex = targetPageStart + clampedRowInPage;
    ClampSelection(&view);
    EnsureSelectionVisible(&view);

    const bool changed = (view.selectedIndex != oldSelected) || (view.firstVisibleIndex != oldFirst);
    if (changed)
    {
        EnsureVisibleReplayMetadataLoaded();
        mod::LogVerbose(
            "ReplayBrowser::MovePage: relative='%s' old=%zu new=%zu oldFirst=%zu first=%zu rows=%zu pageRows=%zu delta=%d rowInPage=%zu targetPage=%zu",
            view.relativeFolderPath.c_str(),
            oldSelected,
            view.selectedIndex,
            oldFirst,
            view.firstVisibleIndex,
            view.entries.size(),
            pageRows,
            deltaPages,
            rowInPage,
            static_cast<std::size_t>(targetPage));
    }
    return changed;
}

bool ReplayBrowser::EnterSelectedDirectory()
{
    if (folderStack_.empty())
    {
        return false;
    }

    const ReplayBrowserView& current = folderStack_.back().view;
    if (current.entries.empty() || current.selectedIndex >= current.entries.size())
    {
        return false;
    }

    const ReplayBrowserEntry& selected = current.entries[current.selectedIndex];
    if (selected.kind != BrowserEntryKind::Directory)
    {
        return false;
    }

    ReplayBrowserView child = {};
    if (!ScanFolder(selected.relativePath, &child))
    {
        mod::Log(
            "ReplayBrowser::EnterSelectedDirectory: failed scan relative='%s'",
            selected.relativePath.c_str());
        return false;
    }

    FolderFrame frame = {};
    frame.view = std::move(child);
    folderStack_.push_back(std::move(frame));
    EnsureVisibleReplayMetadataLoaded();

    mod::Log(
        "ReplayBrowser::EnterSelectedDirectory: entered relative='%s' depth=%zu entries=%zu",
        folderStack_.back().view.relativeFolderPath.c_str(),
        folderStack_.size(),
        folderStack_.back().view.entries.size());
    return true;
}

bool ReplayBrowser::ReturnToParentDirectory()
{
    if (folderStack_.size() <= 1)
    {
        return false;
    }

    const std::string from = folderStack_.back().view.relativeFolderPath;
    folderStack_.pop_back();
    EnsureVisibleReplayMetadataLoaded();

    mod::Log(
        "ReplayBrowser::ReturnToParentDirectory: from='%s' to='%s' depth=%zu",
        from.c_str(),
        folderStack_.back().view.relativeFolderPath.c_str(),
        folderStack_.size());
    return true;
}

const ReplayBrowserView* ReplayBrowser::CurrentView() const
{
    if (folderStack_.empty())
    {
        return nullptr;
    }
    return &folderStack_.back().view;
}

const ReplayBrowserEntry* ReplayBrowser::SelectedEntry() const
{
    const ReplayBrowserView* view = CurrentView();
    if (view == nullptr || view->entries.empty() || view->selectedIndex >= view->entries.size())
    {
        return nullptr;
    }
    return &view->entries[view->selectedIndex];
}

bool ReplayBrowser::HasEntries() const
{
    const ReplayBrowserView* view = CurrentView();
    return view != nullptr && !view->entries.empty();
}

std::size_t ReplayBrowser::VisibleRows() const
{
    return config_.visibleRows;
}

void ReplayBrowser::EnsureVisibleReplayMetadataLoaded()
{
    if (folderStack_.empty())
    {
        return;
    }

    ReplayBrowserView& view = folderStack_.back().view;
    if (view.entries.empty())
    {
        return;
    }

    const std::size_t start = view.firstVisibleIndex;
    const std::size_t end = std::min(start + config_.visibleRows, view.entries.size());
    std::size_t attempted = 0;
    std::size_t success = 0;
    for (std::size_t i = start; i < end; ++i)
    {
        ReplayBrowserEntry& entry = view.entries[i];
        if (entry.kind != BrowserEntryKind::ReplayFile || entry.metadataLoaded)
        {
            continue;
        }

        ++attempted;
        entry.metadataLoaded = true;
        entry.metadata = {};
        entry.playable = false;

        try
        {
            file::ReplayHeaderMetadata metadata = {};
            const bool parsed = file::ParseReplayHeaderMetadata(entry.absolutePathFs, &metadata);
            entry.metadata = metadata;
            if (parsed && entry.characters[0] < 0)
            {
                entry.characters[0] = metadata.p1CharacterId;
                entry.characters[1] = metadata.p2CharacterId;
            }
            entry.playable = parsed
                && file::IsLikelyValidCharacterId(entry.metadata.p1CharacterId)
                && file::IsLikelyValidCharacterId(entry.metadata.p2CharacterId);
            if (entry.playable)
            {
                ++success;
            }
        }
        catch (const std::exception& ex)
        {
            mod::Log(
                "ReplayBrowser::EnsureVisibleReplayMetadataLoaded: exception path='%s' what='%s'",
                PathForLog(entry.absolutePathFs).c_str(),
                ex.what());
        }
        catch (...)
        {
            mod::Log(
                "ReplayBrowser::EnsureVisibleReplayMetadataLoaded: unknown exception path='%s'",
                PathForLog(entry.absolutePathFs).c_str());
        }
    }

    if (attempted > 0)
    {
        mod::LogVerbose(
            "ReplayBrowser::EnsureVisibleReplayMetadataLoaded: relative='%s' start=%zu end=%zu attempted=%zu playable=%zu",
            view.relativeFolderPath.c_str(),
            start,
            end,
            attempted,
            success);
    }
}

std::size_t ReplayBrowser::VisibleStartIndex() const
{
    const ReplayBrowserView* view = CurrentView();
    if (view == nullptr)
    {
        return 0;
    }
    return view->firstVisibleIndex;
}

std::size_t ReplayBrowser::VisibleEndIndexExclusive() const
{
    const ReplayBrowserView* view = CurrentView();
    if (view == nullptr)
    {
        return 0;
    }
    const std::size_t maxEnd = view->firstVisibleIndex + config_.visibleRows;
    return std::min(maxEnd, view->entries.size());
}

std::size_t ReplayBrowser::SelectedVisibleRow() const
{
    const ReplayBrowserView* view = CurrentView();
    if (view == nullptr || view->entries.empty())
    {
        return 0;
    }
    if (view->selectedIndex < view->firstVisibleIndex)
    {
        return 0;
    }
    const std::size_t row = view->selectedIndex - view->firstVisibleIndex;
    const std::size_t maxRow = (config_.visibleRows > 0) ? (config_.visibleRows - 1) : 0;
    return (row > maxRow) ? maxRow : row;
}

bool ReplayBrowser::SetReplaySortOrder(ReplaySortOrder order)
{
    if (config_.replaySortOrder == order)
    {
        return false;
    }
    config_.replaySortOrder = order;

    for (FolderFrame& frame : folderStack_)
    {
        ResortKeepingSelection(&frame.view);
    }
    return true;
}

ReplaySortOrder ReplayBrowser::GetReplaySortOrder() const
{
    return config_.replaySortOrder;
}

const std::filesystem::path& ReplayBrowser::GetRootAbsolutePathFs() const
{
    return rootAbsolutePathFs_;
}

const std::string& ReplayBrowser::GetRootAbsolutePath() const
{
    return rootAbsolutePath_;
}

bool ReplayBrowser::NavigateToAndSelect(
    const std::string& relativeFolder,
    const std::filesystem::path& targetAbsPath)
{
    if (folderStack_.empty())
    {
        mod::Log("ReplayBrowser::NavigateToAndSelect: empty stack");
        return false;
    }

    while (folderStack_.size() > 1)
    {
        folderStack_.pop_back();
    }
    if (!ScanFolder("", &folderStack_[0].view))
    {
        mod::Log("ReplayBrowser::NavigateToAndSelect: root rescan failed");
        return false;
    }

    if (!relativeFolder.empty())
    {
        const std::filesystem::path relPath = Utf8ToPath(relativeFolder);
        for (const std::filesystem::path& compPath : relPath)
        {
            const std::string compStr = ToGenericPathString(compPath);
            if (compStr.empty() || compStr == "." || compStr == "/")
            {
                continue;
            }

            ReplayBrowserView& view = folderStack_.back().view;
            bool entered = false;
            for (std::size_t i = 0; i < view.entries.size(); ++i)
            {
                if (view.entries[i].kind != BrowserEntryKind::Directory)
                {
                    continue;
                }
                const std::string entryName = ToGenericPathString(view.entries[i].absolutePathFs.filename());
                if (LowerAscii(entryName) == LowerAscii(compStr))
                {
                    view.selectedIndex = i;
                    EnsureSelectionVisible(&view);
                    ReplayBrowserView child = {};
                    if (!ScanFolder(view.entries[i].relativePath, &child))
                    {
                        mod::Log(
                            "ReplayBrowser::NavigateToAndSelect: scan failed comp='%s' relative='%s'",
                            compStr.c_str(),
                            view.entries[i].relativePath.c_str());
                        break;
                    }
                    FolderFrame frame = {};
                    frame.view = std::move(child);
                    folderStack_.push_back(std::move(frame));
                    entered = true;
                    mod::Log(
                        "ReplayBrowser::NavigateToAndSelect: entered comp='%s' depth=%zu",
                        compStr.c_str(),
                        folderStack_.size());
                    break;
                }
            }
            if (!entered)
            {
                mod::Log(
                    "ReplayBrowser::NavigateToAndSelect: comp not found='%s', stopping folder nav",
                    compStr.c_str());
                break;
            }
        }
    }

    if (!targetAbsPath.empty())
    {
        ReplayBrowserView& view = folderStack_.back().view;
        bool selected = false;
        for (const bool exact : {true, false})
        {
            for (std::size_t i = 0; i < view.entries.size() && !selected; ++i)
            {
                std::error_code ec;
                selected = exact ? view.entries[i].absolutePathFs == targetAbsPath
                                 : (std::filesystem::equivalent(view.entries[i].absolutePathFs, targetAbsPath, ec) && !ec);
                if (selected)
                {
                    view.selectedIndex = i;
                    EnsureSelectionVisible(&view);
                    mod::Log(
                        "ReplayBrowser::NavigateToAndSelect: selected index=%zu path='%s'",
                        i,
                        PathForLog(targetAbsPath).c_str());
                }
            }
        }
    }

    EnsureVisibleReplayMetadataLoaded();
    mod::Log(
        "ReplayBrowser::NavigateToAndSelect: done folder='%s' depth=%zu sel=%zu",
        folderStack_.back().view.relativeFolderPath.c_str(),
        folderStack_.size(),
        folderStack_.back().view.selectedIndex);
    return true;
}

bool ReplayBrowser::ScanFolder(const std::string& relativeFolderPath, ReplayBrowserView* outView) const
{
    if (outView == nullptr)
    {
        mod::Log("ReplayBrowser::ScanFolder: outView is null");
        return false;
    }

    try
    {
        outView->relativeFolderPath = relativeFolderPath;
        outView->entries.clear();
        outView->all.clear();
        outView->selectedIndex = 0;
        outView->firstVisibleIndex = 0;

        std::filesystem::path absolute = rootAbsolutePathFs_;
        if (!relativeFolderPath.empty())
        {
            absolute /= Utf8ToPath(relativeFolderPath);
        }

        std::error_code ec;
        if (!std::filesystem::exists(absolute, ec) || ec)
        {
            mod::Log(
                "ReplayBrowser::ScanFolder: missing absolute='%s' relative='%s' ec=%d",
                PathForLog(absolute).c_str(),
                relativeFolderPath.c_str(),
                ec.value());
            return false;
        }
        if (!std::filesystem::is_directory(absolute, ec) || ec)
        {
            mod::Log(
                "ReplayBrowser::ScanFolder: not directory absolute='%s' relative='%s' ec=%d",
                PathForLog(absolute).c_str(),
                relativeFolderPath.c_str(),
                ec.value());
            return false;
        }

        std::size_t seenDirs = 0;
        std::size_t seenRepFiles = 0;
        std::size_t skippedFiles = 0;

        for (const std::filesystem::directory_entry& de : std::filesystem::directory_iterator(
                 absolute,
                 std::filesystem::directory_options::skip_permission_denied,
                 ec))
        {
            if (ec)
            {
                mod::Log(
                    "ReplayBrowser::ScanFolder: iterator error absolute='%s' ec=%d",
                    PathForLog(absolute).c_str(),
                    ec.value());
                break;
            }

            try
            {
                const std::filesystem::path absPath = de.path();
                const std::filesystem::path filename = absPath.filename();
                std::string fileName = Utf8PathBestEffort(filename);
                if (fileName.empty())
                {
                    fileName = PathForLog(filename);
                }
                if (fileName.empty())
                {
                    continue;
                }

                if (de.is_directory(ec) && !ec)
                {
                    ReplayBrowserEntry e = {};
                    e.kind = BrowserEntryKind::Directory;
                    e.playable = false;
                    e.displayName = fileName;
                    e.sortKey = LowerAscii(fileName);
                    e.absolutePathFs = absPath;
                    e.relativePath = ToGenericPathString(absPath.lexically_relative(rootAbsolutePathFs_));
                    if (e.relativePath.empty())
                    {
                        e.relativePath = fileName;
                    }
                    outView->entries.push_back(std::move(e));
                    ++seenDirs;
                    continue;
                }

                if (!de.is_regular_file(ec) || ec)
                {
                    ec.clear();
                    continue;
                }

                if (!file::HasRepExtension(absPath))
                {
                    continue;
                }
                if (LowerAscii(fileName) == "__efz_open_alias.rep")
                {
                    continue;
                }
                ++seenRepFiles;
                if (config_.ignoreUnusableReplayFiles && (de.file_size(ec) == 0 || ec))
                {
                    ec.clear();
                    ++skippedFiles;
                    continue;
                }

                ReplayBrowserEntry e = {};
                e.kind = BrowserEntryKind::ReplayFile;
                e.absolutePathFs = absPath;

                std::error_code sizeError;
                e.sizeBytes = de.file_size(sizeError);

                bool concrete = false;
                const std::string stem = replay::name::ExtractReplayStem(fileName);
                replay::name::ParsedConcreteReplayName parsedName = {};
                if (replay::name::TryParseConcreteReplayName(stem, &parsedName))
                {
                    e.players[0] = parsedName.p1Name;
                    e.players[1] = parsedName.p2Name;
                    e.characters[0] = replay::name::ReplayCharacterIdFromName(parsedName.p1Character);
                    e.characters[1] = replay::name::ReplayCharacterIdFromName(parsedName.p2Character);
                    if (e.characters[0] < 0 || e.characters[1] < 0)
                    {
                        e.characters[0] = e.characters[1] = -1;
                    }
                    e.dateKey = std::strtoull((parsedName.dateYmd + parsedName.timeHms).c_str(), nullptr, 10)
                        * (parsedName.timeHms.size() == 4 ? 100u : 1u);
                }
                else
                {
                    (void)replay::name::TryParsePlainReplayName(stem, &e.players[0], &e.players[1]);
                    e.dateKey = LocalDateKey(de.last_write_time(sizeError));
                }
                e.playerKey = LowerAscii(e.players[0]);
                e.displayName = replay::name::FormatReplayDisplayName(stem, config_.prettifyConcreteNames, &concrete);
                if (e.displayName.empty())
                {
                    e.displayName = fileName;
                }
                e.sortKey = LowerAscii(e.displayName);
                e.concreteNameMatched = concrete;
                e.metadata = {};
                e.metadataLoaded = false;
                e.playable = false;

                outView->entries.push_back(std::move(e));
            }
            catch (const std::exception& ex)
            {
                ++skippedFiles;
                mod::Log(
                    "ReplayBrowser::ScanFolder: skipped entry due exception relative='%s' what='%s'",
                    relativeFolderPath.c_str(),
                    ex.what());
            }
            catch (...)
            {
                ++skippedFiles;
                mod::Log(
                    "ReplayBrowser::ScanFolder: skipped entry due unknown exception relative='%s'",
                    relativeFolderPath.c_str());
            }
        }

        SortEntries(outView);

        mod::Log(
            "ReplayBrowser::ScanFolder: relative='%s' entries=%zu dirs=%zu repSeen=%zu skipped=%zu",
            relativeFolderPath.c_str(),
            outView->entries.size(),
            seenDirs,
            seenRepFiles,
            skippedFiles);
        return true;
    }
    catch (const std::exception& ex)
    {
        mod::Log(
            "ReplayBrowser::ScanFolder: fatal exception relative='%s' what='%s'",
            relativeFolderPath.c_str(),
            ex.what());
        return false;
    }
    catch (...)
    {
        mod::Log(
            "ReplayBrowser::ScanFolder: fatal unknown exception relative='%s'",
            relativeFolderPath.c_str());
        return false;
    }
}

void ReplayBrowser::ClampSelection(ReplayBrowserView* view) const
{
    if (view == nullptr || view->entries.empty())
    {
        if (view != nullptr)
        {
            view->selectedIndex = 0;
            view->firstVisibleIndex = 0;
        }
        return;
    }

    const std::size_t maxIndex = view->entries.size() - 1;
    if (view->selectedIndex > maxIndex)
    {
        view->selectedIndex = maxIndex;
    }
    if (view->firstVisibleIndex > maxIndex)
    {
        view->firstVisibleIndex = maxIndex;
    }
}

void ReplayBrowser::EnsureSelectionVisible(ReplayBrowserView* view) const
{
    if (view == nullptr || view->entries.empty())
    {
        if (view != nullptr)
        {
            view->firstVisibleIndex = 0;
        }
        return;
    }

    if (view->selectedIndex < view->firstVisibleIndex)
    {
        view->firstVisibleIndex = view->selectedIndex;
        return;
    }

    const std::size_t visibleRows = (config_.visibleRows > 0) ? config_.visibleRows : 1;
    const std::size_t visibleEndExclusive = view->firstVisibleIndex + visibleRows;
    if (view->selectedIndex >= visibleEndExclusive)
    {
        view->firstVisibleIndex = view->selectedIndex - visibleRows + 1;
        return;
    }
}
}
