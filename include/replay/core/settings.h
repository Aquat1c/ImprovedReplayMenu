#pragma once

#include "replay/core/replay_browser.h"

#include <cstdint>

// User settings in ImprovedReplayMenu.ini next to the DLL ([Settings] Language=auto|en|ja,
// Sort=newest|oldest, BackgroundScroll=on|off, FrameCounter=on|off). Loaded once; every setter writes through.
namespace replay::settings
{
enum class Language : std::uint8_t
{
    Auto = 0,
    English,
    Japanese,
};

void Load();
Language GetLanguage();
void SetLanguage(Language language);
browser::ReplaySortOrder GetSortOrder();
void SetSortOrder(browser::ReplaySortOrder order);
bool GetBackgroundScroll();
void SetBackgroundScroll(bool enabled);
bool GetFrameCounter();
void SetFrameCounter(bool enabled);

// [Rewind] Enabled=0 turns rewind off and leaves Revival's replay keys stock. Keys are DIK codes.
enum class RewindKey : std::uint8_t { Back, Forward, PrevRound, NextRound, ToggleCounter, Count };
bool GetRewindEnabled();
int GetRewindKey(RewindKey key);
void SetRewindKey(RewindKey key, int dik);
void RestoreRewindKeyDefaults();
bool IsExcludedKey(int dik);
bool IsRewindKeyUsedByOther(RewindKey key, int dik);

}
