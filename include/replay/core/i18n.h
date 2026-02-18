#pragma once

#include <cstdint>
#include <string>

namespace replay::i18n
{
enum class Text : std::uint8_t
{
    OptionsTitle,
    RecordReplay,
    Sort,
    SortNewest,
    SortOldest,
    PlayFolder,
    RefreshFolder,
    Language,
    LanguageAuto,
    BackgroundScroll,
    On,
    Off,
    NoReplays,
    About,
    Build,
    RewindKeys,
    KeyBack,
    KeyForward,
    KeyPrevRound,
    KeyNextRound,
    KeysRestore,
    KeysHint,
    KeysRelease,
    KeysPress,
    KeyInUseFmt,
    SortFilterRow,
    SortPlayer,
    SortLargest,
    FilterTitle,
    FilterCharacter,
    FilterCharacter2,
    FilterPlayer,
    FilterAll,
    FilterClear,
    FilterHint,
    FilterCountFmt,
    FilterTagFmt,
    TipRecord,
    TipSort,
    TipPlayFolder,
    TipRefresh,
    TipBackground,
    TipLanguage,
    TipRewindKeys,
    TipKeyBack,
    TipKeyForward,
    TipKeyPrevRound,
    TipKeyNextRound,
    TipKeysRestore,
    TipFilterSort,
    TipFilterCharacter,
    TipFilterCharacter2,
    TipFilterPlayer,
    TipFilterClear,
    HintOptionsAction,
    HintOptionsValue,
    HintOptionsSort,
    HintFilterClear,
    Count,
};

bool IsJapanese();
const char* Tr(Text text);
std::string KeyName(int dik);
}
