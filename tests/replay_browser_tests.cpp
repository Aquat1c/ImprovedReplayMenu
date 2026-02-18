#include "replay/core/menu_motion.h"
#include "replay/core/replay_browser.h"
#include "replay/core/replay_name_format.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
namespace fs = std::filesystem;
using replay::browser::BrowserEntryKind;
using replay::browser::ReplayBrowser;

#define CHECK(expr) do { if (!(expr)) { throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #expr); } } while (0)

std::string Utf8(const fs::path& path)
{
    const auto text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

struct Fixture
{
    fs::path temporary = fs::canonical(fs::temp_directory_path());
    fs::path root = temporary / ("replay-browser-tests-" + std::to_string(GetCurrentProcessId())
        + "-" + std::to_string(GetTickCount64()));

    Fixture() { CHECK(fs::create_directory(root)); }
    ~Fixture()
    {
        // Only remove the uniquely created child of the resolved temporary directory.
        if (root.parent_path() == temporary)
        {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    }
};

void WriteReplay(const fs::path& path)
{
    const char header[] = {1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 0, 0};
    std::ofstream file(path, std::ios::binary);
    file.write(header, sizeof(header));
    file.close();
    CHECK(file.good());
}

void Select(ReplayBrowser& browser, const fs::path& path);

void WriteSized(const fs::path& path, std::size_t size)
{
    std::string bytes(size, '\0');
    bytes[0] = 1;
    bytes[2] = 2;
    bytes[4] = 104;
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    CHECK(file.good());
}

std::vector<std::string> Names(const ReplayBrowser& browser)
{
    std::vector<std::string> names;
    for (const auto& e : browser.CurrentView()->entries)
    {
        const auto text = e.absolutePathFs.filename().u8string();
        names.emplace_back(text.begin(), text.end());
    }
    return names;
}

void CheckMenuMotion()
{
    namespace motion = replay::motion;
    CHECK(motion::RowDrop(0.0) == 0);
    CHECK(motion::RowDrop(180.0) >= 240);
    CHECK(motion::PanelOffset(0.0) == 320);
    CHECK(motion::PanelOffset(180.0) == 0);
    CHECK(motion::SelectedRise(8, 0.0) == 0.0);
    CHECK(96 + 8 * 16 - motion::SelectedRise(8, 180.0) == -16.0);
    CHECK(96 - motion::SelectedRise(0, 180.0) == -16.0);
    CHECK(motion::TextScrollOffset(60.0f, 0) == 0.0f);
    CHECK(motion::TextScrollOffset(60.0f, 1199) == 0.0f);
    CHECK(motion::TextScrollOffset(60.0f, 1700) > 0.0f);
    CHECK(motion::TextScrollOffset(60.0f, 1700) < 60.0f);
    CHECK(motion::TextScrollOffset(60.0f, 3000) == 60.0f);
    CHECK(motion::TextScrollOffset(60.0f, 3400) == 0.0f);
}

void CheckNames()
{
    namespace name = replay::name;
    name::ParsedConcreteReplayName parsed = {};
    CHECK(name::TryParseConcreteReplayName("20141119-2250 EFZ_bot (Akiko) vs EfzPlayer (Misaki)", &parsed));
    CHECK(parsed.timeHms == "2250");
    CHECK(parsed.p1Name == "EFZ_bot");
    CHECK(parsed.p2Character == "Misaki");
    CHECK(name::FormatReplayDisplayName("20141119-2250 A (Akiko) vs B (Mio)", true, nullptr)
        == "2014-11-19 22:50  A (Akiko) vs B (Mio)");
    CHECK(name::TryParseConcreteReplayName("20250321-021538 A vs B (Mai) vs C (Mio)", &parsed));
    CHECK(parsed.p1Name == "A vs B");
    CHECK(!name::TryParseConcreteReplayName("data1-1", &parsed));

    std::string p1, p2;
    CHECK(name::TryParsePlainReplayName("craiv vs rutoa (1)", &p1, &p2));
    CHECK(p1 == "craiv" && p2 == "rutoa");
    CHECK(name::TryParsePlainReplayName("a vs b (c)", &p1, &p2));
    CHECK(p2 == "b (c)");
    CHECK(!name::TryParsePlainReplayName("data1-1", &p1, &p2));
    CHECK(!name::TryParsePlainReplayName("2014 a vs b", &p1, &p2));

    CHECK(name::ReplayCharacterIdFromName("Rumi") == 0);
    CHECK(name::ReplayCharacterIdFromName("Misuzu") == 23);
    CHECK(name::ReplayCharacterIdFromName("nobody") == -1);
    CHECK(name::ReplayCharacterName(22) == nullptr);
    int current = -1;
    int seen = 0;
    do
    {
        current = name::StepReplayCharacterFilter(current, 1);
        CHECK(current == -1 || name::ReplayCharacterName(current) != nullptr);
        ++seen;
    } while (current != -1 && seen < 40);
    CHECK(seen == 24);
    CHECK(name::StepReplayCharacterFilter(-1, -1) == 18);
}

void CheckSortAndFilter(const fs::path& root)
{
    const fs::path dir = root / "library";
    fs::create_directories(dir / "sub");
    WriteSized(dir / "20140101-1200 Bob (Akiko) vs Eve (Misaki).rep", 100);
    WriteSized(dir / "20250101-120000 Eve (Mio) vs Bob (Akiko).rep", 300);
    WriteSized(dir / "20200101-120000 alice (Rumi) vs Carol (Mai).rep", 200);
    WriteSized(dir / "dave vs erin (1).rep", 50);
    WriteSized(dir / "data1-1.rep", 400);
    WriteSized(dir / "header only.rep", 12);
    WriteSized(dir / "empty.rep", 0);

    replay::browser::ReplayBrowserConfig config;
    config.replaySortOrder = replay::browser::ReplaySortOrder::NewestFirst;
    ReplayBrowser browser;
    CHECK(browser.Initialize(Utf8(dir), config));
    const std::string dated[] = {"20250101-120000 Eve (Mio) vs Bob (Akiko).rep",
        "20200101-120000 alice (Rumi) vs Carol (Mai).rep", "20140101-1200 Bob (Akiko) vs Eve (Misaki).rep"};
    auto names = Names(browser);
    CHECK(names.size() == 7);
    CHECK(names[0] == "sub");
    const auto position = [&names](const std::string& wanted) {
        return std::find(names.begin(), names.end(), wanted) - names.begin();
    };
    CHECK(position(dated[0]) < position(dated[1]) && position(dated[1]) < position(dated[2]));

    CHECK(browser.SetReplaySortOrder(replay::browser::ReplaySortOrder::OldestFirst));
    names = Names(browser);
    CHECK(position(dated[2]) < position(dated[1]) && position(dated[1]) < position(dated[0]));

    CHECK(browser.SetReplaySortOrder(replay::browser::ReplaySortOrder::LargestFirst));
    names = Names(browser);
    CHECK(names[1] == "data1-1.rep");
    CHECK(names[2] == dated[0]);

    CHECK(browser.SetReplaySortOrder(replay::browser::ReplaySortOrder::PlayerName));
    names = Names(browser);
    CHECK(names[1] == dated[1]);
    CHECK(position("data1-1.rep") > position("dave vs erin (1).rep"));

    const auto players = browser.PlayerOptions();
    CHECK(players.size() == 6);
    CHECK(players[0] == "alice" && players[1] == "Bob");

    Select(browser, dir / dated[0]);
    replay::browser::ReplayBrowserFilter filter;
    filter.player = "Bob";
    browser.SetFilter(filter);
    CHECK(browser.CurrentView()->shownReplays == 2);
    CHECK(browser.CurrentView()->totalReplays == 6);
    CHECK(browser.SelectedEntry()->absolutePathFs == dir / dated[0]);
    CHECK(Names(browser)[0] == "sub");

    filter = {};
    filter.characters[0] = 16;
    browser.SetFilter(filter);
    CHECK(browser.CurrentView()->shownReplays == 2);

    filter.characters[1] = 7;
    browser.SetFilter(filter);
    CHECK(browser.CurrentView()->shownReplays == 1);
    filter.characters[1] = 11;
    browser.SetFilter(filter);
    CHECK(browser.CurrentView()->shownReplays == 1);
    filter.characters[1] = 16;
    browser.SetFilter(filter);
    CHECK(browser.CurrentView()->shownReplays == 0);
    filter.characters[0] = -1;
    browser.SetFilter(filter);
    CHECK(browser.CurrentView()->shownReplays == 2);
    filter.characters[1] = -1;

    filter.characters[0] = 1;
    browser.SetFilter(filter);
    names = Names(browser);
    CHECK(position("data1-1.rep") != static_cast<std::ptrdiff_t>(names.size()));
    CHECK(position(dated[0]) == static_cast<std::ptrdiff_t>(names.size()));

    filter.player = "nobody";
    browser.SetFilter(filter);
    CHECK(browser.CurrentView()->shownReplays == 0);

    browser.SetFilter({});
    CHECK(browser.CurrentView()->shownReplays == 6);
    CHECK(browser.CurrentView()->all.empty());
    CHECK(browser.RefreshCurrentFolder());
    CHECK(browser.CurrentView()->shownReplays == 6);

    Select(browser, dir / "header only.rep");
    browser.EnsureVisibleReplayMetadataLoaded();
    CHECK(!browser.SelectedEntry()->playable);
}

void Select(ReplayBrowser& browser, const fs::path& path)
{
    const auto* view = browser.CurrentView();
    CHECK(view != nullptr);
    for (std::size_t i = 0; i < view->entries.size(); ++i)
    {
        if (view->entries[i].absolutePathFs == path)
        {
            browser.MoveSelection(static_cast<int>(i) - static_cast<int>(view->selectedIndex));
            CHECK(browser.SelectedEntry()->absolutePathFs == path);
            return;
        }
    }
    throw std::runtime_error("missing browser entry: " + Utf8(path));
}

void CheckSelectedReplay(const ReplayBrowser& browser, const fs::path& path)
{
    const auto* selected = browser.SelectedEntry();
    CHECK(selected != nullptr);
    CHECK(selected->absolutePathFs == path);
    CHECK(selected->kind == BrowserEntryKind::ReplayFile);
    CHECK(selected->playable);
    CHECK(selected->metadataLoaded);
    CHECK(selected->metadata.p1CharacterId == 1);
    CHECK(selected->metadata.p2CharacterId == 2);
    CHECK(selected->metadata.stageId == 3);
    CHECK(selected->metadata.p1Color == 4);
    CHECK(selected->metadata.p2Color == 5);
}

void CheckFolderNavigation(const fs::path& root, const fs::path& relativeFolder)
{
    ReplayBrowser browser;
    CHECK(browser.Initialize(Utf8(root), {}));
    fs::path current = root;
    for (const auto& component : relativeFolder)
    {
        current /= component;
        Select(browser, current);
        CHECK(browser.SelectedEntry()->kind == BrowserEntryKind::Directory);
        CHECK(browser.EnterSelectedDirectory());
    }
    CHECK(browser.CurrentView()->relativeFolderPath == Utf8(relativeFolder));

    const fs::path replay = current / L"\u5bfe\u6226.rep";
    Select(browser, replay);
    CheckSelectedReplay(browser, replay);
    CHECK(browser.RefreshCurrentFolder());
    CheckSelectedReplay(browser, replay);

    CHECK(browser.IsInSubfolder());
    CHECK(browser.ReturnToParentDirectory());
    CHECK(browser.SelectedEntry()->absolutePathFs == current);
    CHECK(browser.EnterSelectedDirectory());
    Select(browser, replay);
    CheckSelectedReplay(browser, replay);

    ReplayBrowser restored;
    CHECK(restored.Initialize(Utf8(root), {}));
    CHECK(restored.NavigateToAndSelect(Utf8(relativeFolder), replay));
    CHECK(restored.CurrentView()->relativeFolderPath == Utf8(relativeFolder));
    CheckSelectedReplay(restored, replay);
    CHECK(restored.ReturnToParentDirectory());
    CHECK(restored.SelectedEntry()->absolutePathFs == current);
}
}

int main()
{
    try
    {
        Fixture fixture;
        const fs::path folders[] = {
            L"ascii",
            L"\u65e5\u672c\u8a9e",
            L"\u65e5\u672c\u8a9e/\u041a\u0438\u0440\u0438\u043b\u043b\u0438\u0446\u0430_\u65e5\u672c\u8a9e"
        };
        for (const auto& folder : folders)
        {
            fs::create_directories(fixture.root / folder);
            WriteReplay(fixture.root / folder / "a.rep");
            WriteReplay(fixture.root / folder / L"\u5bfe\u6226.rep");
        }
        for (const auto& folder : folders)
        {
            CheckFolderNavigation(fixture.root, folder);
        }

        const fs::path unicodeRoot = fixture.root / L"\u4fdd\u5b58_\u041f\u0443\u0442\u044c";
        fs::create_directories(unicodeRoot / "ascii");
        WriteReplay(unicodeRoot / "ascii" / "a.rep");
        WriteReplay(unicodeRoot / "ascii" / L"\u5bfe\u6226.rep");
        CheckFolderNavigation(unicodeRoot, "ascii");
        CheckNames();
        CheckMenuMotion();
        CheckSortAndFilter(fixture.root);
        std::puts("PASS: ASCII, Japanese and mixed nested replay folders enter, refresh, return and restore with wide metadata paths");
        return 0;
    }
    catch (const std::exception& ex)
    {
        std::fprintf(stderr, "FAIL: %s\n", ex.what());
        return 1;
    }
}
