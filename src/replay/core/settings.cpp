#include "replay/core/settings.h"

#include "logger.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>

namespace replay::settings
{
namespace
{
Language g_language = Language::Auto;
browser::ReplaySortOrder g_sortOrder = browser::ReplaySortOrder::NewestFirst;
bool g_backgroundScroll = true;
bool g_rewindEnabled = true;
int g_rewindKeys[static_cast<int>(RewindKey::Count)] = {};
const wchar_t* const kRewindKeyNames[] = {L"KeyBack", L"KeyForward", L"KeyPrevRound", L"KeyNextRound"};

int DefaultKeyFor(wchar_t ch, UINT vk, int fallback)
{
    const SHORT result = VkKeyScanW(ch);
    if (result != -1 && (result & 0xFF00) == 0)
    {
        vk = static_cast<UINT>(result & 0xFF);
    }
    const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    return (scan != 0) ? static_cast<int>(scan) : fallback;
}

int DefaultRewindKey(RewindKey key)
{
    switch (key)
    {
    case RewindKey::Back: return DefaultKeyFor(L'-', VK_OEM_MINUS, 0x0C);
    case RewindKey::Forward: return 0x19; // P
    case RewindKey::PrevRound: return 0x0E; // Backspace
    default: return DefaultKeyFor(L'\\', VK_OEM_5, 0x2B);
    }
}

std::wstring IniPath()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&IniPath),
        &module);
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring result(path, (length < MAX_PATH) ? length : 0);
    const std::size_t slash = result.find_last_of(L"\\/");
    result.resize((slash == std::wstring::npos) ? 0 : slash + 1);
    return result + L"ImprovedReplayMenu.ini";
}

void Write(const wchar_t* key, const wchar_t* value, const wchar_t* section = L"Settings")
{
    if (!WritePrivateProfileStringW(section, key, value, IniPath().c_str()))
    {
        mod::Log("Settings: write failed key='%ls' err=%lu", key, GetLastError());
    }
}
}

void Load()
{
    const std::wstring path = IniPath();
    wchar_t value[32] = {};
    GetPrivateProfileStringW(L"Settings", L"Language", L"auto", value, 32, path.c_str());
    g_language = (_wcsicmp(value, L"ja") == 0) ? Language::Japanese
        : (_wcsicmp(value, L"en") == 0)        ? Language::English
                                               : Language::Auto;
    GetPrivateProfileStringW(L"Settings", L"Sort", L"newest", value, 32, path.c_str());
    g_sortOrder = (_wcsicmp(value, L"oldest") == 0) ? browser::ReplaySortOrder::OldestFirst
        : (_wcsicmp(value, L"player") == 0)             ? browser::ReplaySortOrder::PlayerName
        : (_wcsicmp(value, L"size") == 0)               ? browser::ReplaySortOrder::LargestFirst
                                                        : browser::ReplaySortOrder::NewestFirst;
    GetPrivateProfileStringW(L"Settings", L"BackgroundScroll", L"on", value, 32, path.c_str());
    g_backgroundScroll = _wcsicmp(value, L"off") != 0;
    g_rewindEnabled = GetPrivateProfileIntW(L"Rewind", L"Enabled", 1, path.c_str()) != 0;
    for (int i = 0; i < static_cast<int>(RewindKey::Count); ++i)
    {
        const auto key = static_cast<RewindKey>(i);
        g_rewindKeys[i] = DefaultRewindKey(key);
        if (GetPrivateProfileStringW(L"Rewind", kRewindKeyNames[i], L"", value, 32, path.c_str()) != 0)
        {
            const int dik = static_cast<int>(wcstol(value, nullptr, 0));
            if (!IsExcludedKey(dik))
            {
                g_rewindKeys[i] = dik;
            }
        }
    }
    for (int i = 0; i < static_cast<int>(RewindKey::Count); ++i)
    {
        if (IsRewindKeyUsedByOther(static_cast<RewindKey>(i), g_rewindKeys[i]))
        {
            mod::Log("Settings: duplicate rewind keys, using defaults");
            for (int k = 0; k < static_cast<int>(RewindKey::Count); ++k)
            {
                g_rewindKeys[k] = DefaultRewindKey(static_cast<RewindKey>(k));
            }
            break;
        }
    }
    mod::Log("Settings: loaded language=%d sort=%d scroll=%d", static_cast<int>(g_language),
        static_cast<int>(g_sortOrder), g_backgroundScroll ? 1 : 0);
}

bool GetRewindEnabled()
{
    return g_rewindEnabled;
}

int GetRewindKey(RewindKey key)
{
    return g_rewindKeys[static_cast<int>(key)];
}

void SetRewindKey(RewindKey key, int dik)
{
    g_rewindKeys[static_cast<int>(key)] = dik;
    wchar_t value[16] = {};
    swprintf_s(value, L"0x%02X", dik);
    Write(kRewindKeyNames[static_cast<int>(key)], value, L"Rewind");
}

void RestoreRewindKeyDefaults()
{
    for (int i = 0; i < static_cast<int>(RewindKey::Count); ++i)
    {
        SetRewindKey(static_cast<RewindKey>(i), DefaultRewindKey(static_cast<RewindKey>(i)));
    }
}

bool IsExcludedKey(int dik)
{
    if (dik <= 0 || dik >= 256)
    {
        return true;
    }
    switch (dik)
    {
    case 0x01: // Esc
    case 0x1D: case 0x2A: case 0x36: case 0x38: case 0x9D: case 0xB8: // Ctrl, Shift, Alt
    case 0xDB: case 0xDC: // Win
    case 0x3E: case 0x3F: case 0x40: case 0x41: case 0x42: // F4-F8: EFZ battle hotkeys
    case 0x57: // F11: DisplayManager
    case 0x3A: case 0x70: case 0x79: case 0x7B: case 0x94: // lock and IME keys
        return true;
    default:
        return false;
    }
}

bool IsRewindKeyUsedByOther(RewindKey key, int dik)
{
    for (int i = 0; i < static_cast<int>(RewindKey::Count); ++i)
    {
        if (i != static_cast<int>(key) && g_rewindKeys[i] == dik)
        {
            return true;
        }
    }
    return false;
}

bool GetBackgroundScroll()
{
    return g_backgroundScroll;
}

void SetBackgroundScroll(bool enabled)
{
    g_backgroundScroll = enabled;
    Write(L"BackgroundScroll", enabled ? L"on" : L"off");
}

Language GetLanguage()
{
    return g_language;
}

void SetLanguage(Language language)
{
    g_language = language;
    Write(L"Language", (language == Language::Japanese) ? L"ja" : (language == Language::English) ? L"en" : L"auto");
}

browser::ReplaySortOrder GetSortOrder()
{
    return g_sortOrder;
}

void SetSortOrder(browser::ReplaySortOrder order)
{
    g_sortOrder = order;
    Write(L"Sort", (order == browser::ReplaySortOrder::OldestFirst) ? L"oldest"
            : (order == browser::ReplaySortOrder::PlayerName)       ? L"player"
            : (order == browser::ReplaySortOrder::LargestFirst)     ? L"size"
                                                                    : L"newest");
}
}
