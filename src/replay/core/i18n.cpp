#include "replay/core/i18n.h"

#include "replay/core/settings.h"

#include <windows.h>

#include <cstdio>

namespace replay::i18n
{
namespace
{
constexpr const char* kEnglish[] = {
    "Options",
    "Record Replay",
    "Sort",
    "Newest first",
    "Oldest first",
    "Play Folder",
    "Refresh Folder",
    "Language",
    "Auto",
    "Scrolling BG",
    "On",
    "Off",
    "No replays",
    "About",
    "Build",
    "Rewind keys",
    "Back / Rewind",
    "Forward / FF",
    "Restart round",
    "Skip round",
    "Restore defaults",
    "Confirm: Rebind  D / Cancel: Back",
    "Release all keys...",
    "Press a key (Esc: Cancel)",
    "%s is already in use",
    "Sort/Filter",
    "Player name",
    "Largest",
    "Sort / Filter",
    "Character 1",
    "Character 2",
    "Player",
    "All",
    "Clear filters",
    "Left/Right: Change  D / Cancel: Back",
    "%u of %u replays",
    "Filter: %s  %u of %u",
    "Start a new match and record it.",
    "Change the list order, or show only some replays.",
    "Play the replays in this folder, one after another.",
    "Read this folder again to pick up new files.",
    "Turn the moving background on or off.",
    "Choose the language of the menu.",
    "Change the keys used while a replay plays.",
    "Tap for one frame back. Hold to rewind, getting faster.",
    "Tap for one frame forward. Hold to fast forward, getting faster.",
    "Go to the start of this round, or the one before early in a round.",
    "Skip to the start of the next round.",
    "Put all four keys back to their defaults.",
    "The order of the replays in the list.",
    "Show replays with this character on either side.",
    "Pick a second character to show only that matchup.",
    "Show replays with this player on either side.",
    "Remove both filters and show every replay.",
    "Confirm: Select  D / Cancel: Close",
    "Left/Right: Change  D / Cancel: Close",
    "Left/Right: Sort  Confirm: Filters  D / Cancel: Close",
    "Confirm: Clear  D / Cancel: Back",
};
constexpr const char* kJapanese[] = {
    "オプション",
    "リプレイを録画",
    "並び順",
    "新しい順",
    "古い順",
    "フォルダを連続再生",
    "フォルダを再読み込み",
    "言語",
    "自動",
    "背景スクロール",
    "オン",
    "オフ",
    "リプレイがありません",
    "バージョン情報",
    "ビルド",
    "巻き戻しキー",
    "1F戻る / 巻き戻し",
    "1F進む / 早送り",
    "ラウンド先頭へ",
    "次のラウンドへ",
    "初期設定に戻す",
    "決定: 変更  D / キャンセル: 戻る",
    "すべてのキーを離してください",
    "キーを押してください (Esc: 取消)",
    "%s は使用中です",
    "並び替え/絞り込み",
    "プレイヤー名",
    "サイズ順",
    "並び替え / 絞り込み",
    "キャラクター1",
    "キャラクター2",
    "プレイヤー",
    "すべて",
    "絞り込みを解除",
    "左右: 変更  D / キャンセル: 戻る",
    "%u / %u 件",
    "絞り込み: %s  %u / %u 件",
    "新しい対戦を始めて録画します。",
    "リストの順番を変更したり、表示するリプレイを絞り込みます。",
    "このフォルダのリプレイを順番に再生します。",
    "フォルダを読み直して新しいファイルを表示します。",
    "背景の動きをオン / オフします。",
    "メニューの言語を選びます。",
    "リプレイ再生中に使うキーを変更します。",
    "短押しで1フレーム戻ります。長押しで巻き戻し (だんだん速くなります)。",
    "短押しで1フレーム進みます。長押しで早送り (だんだん速くなります)。",
    "このラウンドの先頭へ。ラウンド開始直後は1つ前のラウンドへ。",
    "次のラウンドの先頭へスキップします。",
    "4つのキーをすべて初期設定に戻します。",
    "リストに表示するリプレイの順番です。",
    "どちらかの側がこのキャラクターのリプレイを表示します。",
    "2人目を選ぶと、その組み合わせのリプレイだけを表示します。",
    "どちらかの側がこのプレイヤーのリプレイを表示します。",
    "両方の絞り込みを解除して、すべてのリプレイを表示します。",
    "決定: 選択  D / キャンセル: 閉じる",
    "左右: 変更  D / キャンセル: 閉じる",
    "左右: 並び順  決定: 絞り込み  D / キャンセル: 閉じる",
    "決定: 解除  D / キャンセル: 戻る",
};
static_assert(sizeof(kEnglish) / sizeof(kEnglish[0]) == static_cast<std::size_t>(Text::Count));
static_assert(sizeof(kJapanese) / sizeof(kJapanese[0]) == static_cast<std::size_t>(Text::Count));
}

bool IsJapanese()
{
    switch (settings::GetLanguage())
    {
    case settings::Language::Japanese: return true;
    case settings::Language::English: return false;
    default: return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_JAPANESE;
    }
}

const char* Tr(Text text)
{
    const auto index = static_cast<std::size_t>(text);
    return IsJapanese() ? kJapanese[index] : kEnglish[index];
}
std::string KeyName(int dik)
{
    switch (dik)
    {
    case 0x0E: return "Backspace";
    case 0x0F: return "Tab";
    case 0x1C: return "Enter";
    case 0x39: return "Space";
    case 0x45: return "Num Lock";
    case 0x46: return "Scroll Lock";
    case 0x37: return "Num *";
    case 0x4A: return "Num -";
    case 0x4E: return "Num +";
    case 0x53: return "Num .";
    case 0xB5: return "Num /";
    case 0x9C: return "Num Enter";
    case 0x47: return "Num 7";
    case 0x48: return "Num 8";
    case 0x49: return "Num 9";
    case 0x4B: return "Num 4";
    case 0x4C: return "Num 5";
    case 0x4D: return "Num 6";
    case 0x4F: return "Num 1";
    case 0x50: return "Num 2";
    case 0x51: return "Num 3";
    case 0x52: return "Num 0";
    case 0xC5: return "Pause";
    case 0xC7: return "Home";
    case 0xC9: return "Page Up";
    case 0xCF: return "End";
    case 0xD1: return "Page Down";
    case 0xD2: return "Insert";
    case 0xD3: return "Delete";
    case 0xC8: return "Up";
    case 0xCB: return "Left";
    case 0xCD: return "Right";
    case 0xD0: return "Down";
    case 0x57: return "F11";
    case 0x58: return "F12";
    case 0x7D: return "Yen";
    case 0x73: return "Ro";
    default: break;
    }
    char text[16] = {};
    if (dik >= 0x3B && dik <= 0x44)
    {
        std::snprintf(text, sizeof(text), "F%d", dik - 0x3A);
        return text;
    }
    const UINT vk = MapVirtualKeyW(static_cast<UINT>(dik), MAPVK_VSC_TO_VK);
    const UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFFFFFFu;
    if (ch == '\\')
    {
        return "Backslash"; // the text fonts have no glyph for it
    }
    if (ch >= 0x21 && ch < 0x7F)
    {
        text[0] = static_cast<char>(ch);
        return text;
    }
    std::snprintf(text, sizeof(text), "Key %02X", static_cast<unsigned>(dik));
    return text;
}
}
