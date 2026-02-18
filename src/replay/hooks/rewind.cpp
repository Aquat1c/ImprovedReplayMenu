#include "replay/hooks/rewind.h"

#include "crash_handler.h"
#include "logger.h"
#include "replay/core/hook_install.h"
#include "replay/core/key_capture.h"
#include "replay/core/settings.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Refuses to install unless the build's timestamp, vtable entries and entry signatures match
// exactly. Everything is keyed to the replay session's own vtable, so nothing runs in
// Practice or netplay, and Revival's bindings are read but never written.
namespace
{
using TickFn = void(__fastcall*)(void* sess, void* edx);                 // plain ret
using ReplayIoFn = void(__fastcall*)(void* sess, void* edx, void* slot); // ret 4
using PauseSetFn = void(__fastcall*)(void* sess, void* edx, int paused);   // ret 4
using RenderToggleFn = void(__fastcall*)(void* ctx, void* edx, int on);    // ret 4
using PlayFn = char(__fastcall*)(void* sess, void* edx, unsigned id, int f); // ret 8
using KeyHandlerFn = std::uintptr_t(__fastcall*)(void* sess, void* edx, int key); // ret 4

struct RewindProfile
{
    const char* name;
    std::uint32_t ts, sizeOfImage;
    std::uint32_t vtableRva, tickSlot; // vtable[tickSlot] must == base + tickRva
    std::uint32_t tickRva, replaySaveRva, replayLoadRva, slotCtorRva;
    std::uint32_t ctxRva, ctxSysOff; // sys = *(u32*)(base + ctxRva + ctxSysOff)
    bool gcc;                        // j builds: libstdc++ slot + GCC descriptor layout
    const char *sigTick, *sigSave, *sigLoad, *sigCtor;
    std::uint32_t pauseSetRva, renderToggleRva, playRva, playSlot, keyHandlerRva, keySlot;
    std::uint32_t stepCmpRva, cfgShift; // Revival's step binding: *(int*)(cfg + 0x110 + cfgShift)
    const char *sigPause, *sigRender, *sigPlay, *sigKey, *sigStepCmp;
};

#define S1_TICK_MS(xx) "55 8B EC 83 E4 F8 83 EC 28 56 8B F1 57 8D 7E 60 E8 " xx " FF FF 39 07 74"
#define S1_SAVE_MS(xx) "55 8B EC 83 EC 1C 56 57 8B 7D 08 8B F1 8B CF E8 " xx " FF FF 6A 00 8B CF"
#define S1_LOAD_MS(xx) "55 8B EC 53 56 57 6A 03 C6 41 1A 00 E8 " xx " FF FF 8B 75 08 8B F8 8B CE"
// Extended past the generic SEH prologue up to the descriptor allocation (push 0x218).
#define S1_CTOR_MS                                                                                     \
    "55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 83 EC 08 56 57 8B F9 89 " \
    "7D F0 8D 4F 04 E8 ?? ?? F9 FF 8D 4F 10 C7 45 FC 00 00 00 00 E8 ?? ?? F9 FF 68 ?? ?? ?? ?? 8D 4F " \
    "10 C6 45 FC 01 E8 ?? ?? F9 FF 68 18 02 00 00"
#define S1_TICK_J "57 8D 7C 24 08 83 E4 F8 FF 77 FC 55 89 E5 57 56 53 89 CB 83 EC 24 8B 71"
#define S1_SAVE_J "57 8D 7C 24 08 83 E4 F8 FF 77 FC 55 89 E5 57 53 89 CB 83 C3 08 83 EC 18"
#define S1_LOAD_J "57 8D 7C 24 08 83 E4 F8 FF 77 FC 55 89 E5 57 53 83 EC 28 C6 41 1A 00 8B"
#define S1_CTOR_J "55 8D 41 18 89 E5 83 EC 20 89 5D F4 89 CB 8D 49 10 89 75 F8 C7 41 F4 00"

#define S1_PAUSE_MS "55 8B EC 8A 45 08 56 8B F1 B9 ?? ?? ?? ?? 88 46 18 84 C0 74 1E 6A 00 E8"
#define S1_PAUSE_J "53 89 CB 83 EC 04 0F B6 44 24 0C 88 41 18 84 C0 74 4E C7 04 24 00 00 00"
#define S1_RENDER_MS "55 8B EC 8A 45 08 83 EC 0C 38 05 ?? ?? ?? ?? 0F 84 00 01 00 00 A2 ?? ??"
#define S1_RENDER_J "83 EC 38 0F B6 44 24 3C 38 41 15 74 19 89 5C 24 34 8D 59 78 88 41 15 84"
#define S1_RENDER_J8 "83 EC 38 0F B6 44 24 3C 38 41 15 74 19 89 5C 24 34 8D 59 74 88 41 15 84"
#define S1_PLAY_MS(xx) "55 8B EC 56 8B F1 57 8D 4E 68 E8 " xx " FF FF 8B 55 08 85 C0 74 38 52 E8"
#define S1_PLAY_J "55 89 E5 57 8D 79 68 56 89 CE 89 F9 53 83 EC 20 8B 45 0C 0F B7 5D 08 89"
#define S1_KEY_MS "55 8B EC 83 E4 C0 83 EC 34 53 56 57 8B F1 E8 2D FF FF FF 84 C0 75 09 E8"
#define S1_KEY_J "57 8D 7C 24 08 83 E4 F8 FF 77 FC 55 89 E5 57 56 89 FE 89 CF B9 ?? ?? ??"
#define S1_STEP_EH "3B 98 10 01 00 00 75 14 6A 00 B9"
#define S1_STEP_I "3B 98 14 01 00 00 75 14 6A 00 B9"
#define S1_STEP_J "39 98 14 01 00 00 0F 85 0C FD FF FF C6 47 18 01"

constexpr RewindProfile kProfiles[] = {
    {"1.02e", 0x5EA876B0u, 0xB2000u, 0x90428u, 2, 0x77870u, 0x770C0u, 0x77060u, 0x6ECF0u, 0xA0760u, 0x58u, false,
        S1_TICK_MS("6B 52"), S1_SAVE_MS("0C 79"), S1_LOAD_MS("7F 50"), S1_CTOR_MS,
        0x76CB0u, 0x6C740u, 0x76C40u, 6, 0x774D0u, 3, 0x776E8u, 0, S1_PAUSE_MS, S1_RENDER_MS, S1_PLAY_MS("81 8B"), S1_KEY_MS, S1_STEP_EH},
    {"1.02f", 0x5F8C58A3u, 0xB2000u, 0x90428u, 2, 0x778A0u, 0x770F0u, 0x77090u, 0x6ED20u, 0xA0760u, 0x58u, false,
        S1_TICK_MS("3B 52"), S1_SAVE_MS("DC 78"), S1_LOAD_MS("4F 50"), S1_CTOR_MS,
        0x76CE0u, 0x6C740u, 0x76C70u, 6, 0x77500u, 3, 0x77718u, 0, S1_PAUSE_MS, S1_RENDER_MS, S1_PLAY_MS("81 8B"), S1_KEY_MS, S1_STEP_EH},
    {"1.02f-fs", 0x6085E718u, 0xB2000u, 0x90428u, 2, 0x77B10u, 0x77360u, 0x77300u, 0x6EF30u, 0xA0760u, 0x58u, false,
        S1_TICK_MS("DB 51"), S1_SAVE_MS("7C 78"), S1_LOAD_MS("EF 4F"), S1_CTOR_MS,
        0x76F50u, 0x6C950u, 0x76EE0u, 6, 0x77770u, 3, 0x77988u, 0, S1_PAUSE_MS, S1_RENDER_MS, S1_PLAY_MS("21 8B"), S1_KEY_MS, S1_STEP_EH},
    {"1.02g", 0x6240CE73u, 0xB2000u, 0x90428u, 2, 0x77B40u, 0x77390u, 0x77330u, 0x6EF30u, 0xA0760u, 0x58u, false,
        S1_TICK_MS("AB 51"), S1_SAVE_MS("4C 78"), S1_LOAD_MS("BF 4F"), S1_CTOR_MS,
        0x76F80u, 0x6C950u, 0x76F10u, 6, 0x777A0u, 3, 0x779B8u, 0, S1_PAUSE_MS, S1_RENDER_MS, S1_PLAY_MS("F1 8A"), S1_KEY_MS, S1_STEP_EH},
    {"1.02h", 0x62929371u, 0xB2000u, 0x90568u, 2, 0x78310u, 0x77B60u, 0x77B00u, 0x6F5A0u, 0xA0780u, 0x64u, false,
        S1_TICK_MS("1B 50"), S1_SAVE_MS("EC 76"), S1_LOAD_MS("2F 4E"), S1_CTOR_MS,
        0x77750u, 0x6CF90u, 0x776E0u, 6, 0x77F70u, 3, 0x78188u, 0, S1_PAUSE_MS, S1_RENDER_MS, S1_PLAY_MS("91 89"), S1_KEY_MS, S1_STEP_EH},
    {"1.02i", 0x63BF27EAu, 0xB3000u, 0x91658u, 2, 0x788F0u, 0x78140u, 0x780E0u, 0x6F8F0u, 0xA1790u, 0x64u, false,
        S1_TICK_MS("0B 4D"), S1_SAVE_MS("5C 74"), S1_LOAD_MS("1F 4B"), S1_CTOR_MS,
        0x77D30u, 0x6D260u, 0x77CC0u, 6, 0x78550u, 3, 0x78768u, 4, S1_PAUSE_MS, S1_RENDER_MS, S1_PLAY_MS("01 87"), S1_KEY_MS, S1_STEP_I},
    {"1.02j", 0x6A36A6AEu, 0x1D9000u, 0x16FF50u, 3, 0x74920u, 0x748A0u, 0x74800u, 0x7CBF0u, 0x14E8C0u, 0x64u, true,
        S1_TICK_J, S1_SAVE_J, S1_LOAD_J, S1_CTOR_J,
        0x730E0u, 0x777A0u, 0x72F80u, 7, 0x73190u, 4, 0x7356Du, 4, S1_PAUSE_J, S1_RENDER_J, S1_PLAY_J, S1_KEY_J, S1_STEP_J},
    {"1.02j-0802", 0x6A6F0F7Bu, 0x1D8000u, 0x16FC30u, 3, 0x757C0u, 0x75740u, 0x756A0u, 0x7DC00u, 0x14E8C0u, 0x60u, true,
        S1_TICK_J, S1_SAVE_J, S1_LOAD_J, S1_CTOR_J,
        0x73F80u, 0x78620u, 0x73E20u, 7, 0x74030u, 4, 0x7440Du, 4, S1_PAUSE_J, S1_RENDER_J8, S1_PLAY_J, S1_KEY_J, S1_STEP_J},
};

// efz.exe (fixed image base) globals.
constexpr std::uintptr_t kVaScreenIndex = 0x790148;  // dword, 3 = battle
constexpr std::uintptr_t kVaBattle = 0x79011C;       // battle object
constexpr std::uintptr_t kVaScreen0 = 0x790110;      // +0x43C == 4: replay mode
constexpr std::uintptr_t kVaFirstRecord = 0x7B1290;  // first saved record
constexpr std::uintptr_t kVaDirectDraw = 0x7B0960;   // coreSave calls both vtables unchecked
constexpr std::uintptr_t kVaDirectDraw2 = 0x7B0964;

constexpr int kMaxRecs = 16;

template <typename T>
T Rd(std::uintptr_t address)
{
    return *reinterpret_cast<const T*>(address);
}

enum class RecClass { First, Battle, Battle440, Frame, Sys, Char1, Char2, Palette, Stage, Surface, Unknown };

struct Rec
{
    std::uintptr_t node; // record {dst, data, size} lives at node+8
    std::uintptr_t dst;
    const std::uint8_t* data;
    std::uint32_t size;
    RecClass cls;
};

struct Snapshot
{
    Rec recs[kMaxRecs];
    int count;
};

struct Live
{
    std::uintptr_t sess, battle, char1, char2, sys;
};

const RewindProfile* g_prof = nullptr;
std::uintptr_t g_base = 0;
bool g_s2Disabled = false;
TickFn g_origTick = nullptr;
ReplayIoFn g_replayLoad = nullptr;

Live g_live = {};

double MsSince(const LARGE_INTEGER& start)
{
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return static_cast<double>(now.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

int Nibble(char c)
{
    return (c <= '9') ? (c - '0') : ((c | 0x20) - 'a' + 10);
}

bool MatchSig(const char* fn, std::uint32_t rva, const char* sig)
{
    const auto* code = reinterpret_cast<const std::uint8_t*>(g_base + rva);
    unsigned offset = 0;
    for (const char* p = sig; p[0] != '\0' && p[1] != '\0'; p += (p[2] == ' ') ? 3 : 2, ++offset)
    {
        if (p[0] == '?')
        {
            continue;
        }
        const unsigned want = static_cast<unsigned>((Nibble(p[0]) << 4) | Nibble(p[1]));
        if (code[offset] != want)
        {
            mod::Log("Rewind: disabled reason=signature ts=0x%08X fn=%s off=%u got=%02X want=%02X", g_prof->ts, fn,
                offset, code[offset], want);
            return false;
        }
    }
    return true;
}

RecClass Classify(std::uintptr_t dst, std::uint32_t size)
{
    if (dst == kVaFirstRecord && size == 4) return RecClass::First;
    if (dst == g_live.battle) return RecClass::Battle;
    if (dst == g_live.battle + 0x440) return RecClass::Battle440;
    if (dst == g_live.sess + 8) return RecClass::Frame;
    if (dst == g_live.char1) return RecClass::Char1;
    if (dst == g_live.char2) return RecClass::Char2;
    if (size == 0x142F0) return RecClass::Sys;
    if (size == 0x400) return RecClass::Palette;
    if (size == 0xE0 || size == 0x1C0) return RecClass::Stage;
    if (size >= 0x10000) return RecClass::Surface;
    return RecClass::Unknown;
}

bool CollectAt(std::uintptr_t slot, Snapshot& snap)
{
    snap.count = 0;
    const auto desc = Rd<std::uintptr_t>(slot);
    if (desc == 0)
    {
        return false;
    }
    const std::uintptr_t head = g_prof->gcc ? desc + 0x200 : Rd<std::uintptr_t>(desc + 0x200);
    if (head == 0)
    {
        return false;
    }
    for (auto node = Rd<std::uintptr_t>(head); node != head; node = Rd<std::uintptr_t>(node))
    {
        if (node == 0 || snap.count == kMaxRecs)
        {
            return false;
        }
        Rec& rec = snap.recs[snap.count++];
        rec.node = node;
        rec.dst = Rd<std::uint32_t>(node + 8);
        rec.data = reinterpret_cast<const std::uint8_t*>(Rd<std::uint32_t>(node + 12));
        rec.size = Rd<std::uint32_t>(node + 16);
        rec.cls = Classify(rec.dst, rec.size);
    }
    return true;
}

const Rec* Find(const Snapshot& snap, RecClass cls)
{
    for (int i = 0; i < snap.count; ++i)
    {
        if (snap.recs[i].cls == cls) return &snap.recs[i];
    }
    return nullptr;
}

struct SlotExtras
{
    std::uint32_t used, cap, pos, rngLen, vecLen;
    const char* rng;
    const std::uint8_t* vec;
    std::uint16_t fcw;
};

SlotExtras ReadExtrasAt(std::uintptr_t slot)
{
    const auto desc = Rd<std::uintptr_t>(slot);
    const std::uintptr_t d = desc + (g_prof->gcc ? 4u : 0u); // GCC fields sit one dword later
    SlotExtras x = {};
    x.used = Rd<std::uint32_t>(d + 0x20C) - Rd<std::uint32_t>(d + 0x208);
    x.cap = Rd<std::uint32_t>(d + 0x210);
    x.pos = Rd<std::uint32_t>(slot + 0x28);
    x.fcw = Rd<std::uint16_t>(desc);
    if (g_prof->gcc)
    {
        x.rng = Rd<const char*>(slot + 0x10);
        x.rngLen = Rd<std::uint32_t>(slot + 0x14);
    }
    else
    {
        x.rngLen = Rd<std::uint32_t>(slot + 0x20);
        x.rng = (Rd<std::uint32_t>(slot + 0x24) >= 16) ? Rd<const char*>(slot + 0x10)
                                                       : reinterpret_cast<const char*>(slot + 0x10);
    }
    x.vec = Rd<const std::uint8_t*>(slot + 4);
    x.vecLen = static_cast<std::uint32_t>(Rd<const std::uint8_t*>(slot + 8) - x.vec);
    return x;
}

void Disable(const char* reason)
{
    mod::Log("Rewind: FATAL %s; disabled", reason);
    g_s2Disabled = true;
}

const char* RefreshLive(std::uintptr_t sess)
{
    const auto battle = Rd<std::uintptr_t>(kVaBattle);
    if (battle == 0) return "battle";
    std::uintptr_t chars[2] = {};
    for (int i = 0; i < 2; ++i)
    {
        const auto outer = Rd<std::uintptr_t>(battle + 0x14 + 4 * i);
        chars[i] = (outer != 0) ? Rd<std::uintptr_t>(outer) : 0;
        if (chars[i] == 0) return "character";
    }
    const auto sys = Rd<std::uintptr_t>(g_base + g_prof->ctxRva + g_prof->ctxSysOff);
    if (sys == 0) return "sys";
    g_live = {sess, battle, chars[0], chars[1], sys};
    return nullptr;
}

bool IsStageType10()
{
    const auto stage = Rd<std::uintptr_t>(g_live.sys + 0x137C);
    return stage != 0 && Rd<std::uint16_t>(stage + 4) == 10; // allocates 0x70, Revival copies 0xE0
}

using replay::input::KeyEvent;
using replay::input::KeyTimer;
using replay::settings::RewindKey;

constexpr std::uintptr_t kVaInputMgr = 0x7B0968;
constexpr std::uint32_t kMgrKeyBindings = 448; // 16 u16 per player pair: DIK code in the low byte
constexpr std::uint32_t kMgrKeyboard = 488;    // IDirectInputDevice8*
constexpr std::uint32_t kSysParity = 0x1368;
constexpr std::uint32_t kSysRoundIntro = 0x1350;
constexpr std::uint32_t kSysKo = 0x1354;
constexpr std::uint32_t kSysSlowTrigger = 0x1427C;
constexpr std::uint32_t kSysGameMode = 0x1364; // 5 = replay
constexpr std::uint32_t kSysIoMode = 0x14283;
constexpr std::uint32_t kSysReader = 0x14284;
constexpr std::uint32_t kBattleFadeIn = 0x2C;
constexpr std::uint32_t kBattleExit = 0x2D;
constexpr std::uint32_t kBattleSubSteps = 0x578;
constexpr std::uint32_t kBattleF8Pause = 0x588;
constexpr std::uint32_t kSessCfg = 4;
constexpr std::uint32_t kSessFrame = 8;
constexpr std::uint32_t kSessPaused = 0x18;
constexpr std::uint32_t kSessTakeover = 0x1A;
constexpr std::uint32_t kSessQuit = 0x1B;
constexpr std::uint32_t kSessSide = 0x24;
constexpr std::uint32_t kSessManualSlot = 0x28;
constexpr std::uint32_t kSessRing = 0x90;
constexpr std::uint32_t kSessRingCap = 0x9C;
constexpr std::uint32_t kSlotSize = 0x2C;
constexpr int kKeyCount = static_cast<int>(RewindKey::Count);

constexpr int kMaxHidden = 128;
constexpr DWORD kHoldMs = 250;
constexpr DWORD kHoldRampMs = 500;
constexpr double kFfBudgetMs = 10.0;
constexpr int kScanTicks = 64;
constexpr double kScanBudgetMs = 8.0;
constexpr DWORD kTagMs = 1000;
constexpr int kPrevRoundGrace = 60;
constexpr int kTakeoverKoFrames = 90;
constexpr int kTakeoverGraceTicks = 30; // a pad cannot be seen through the mask, so it only gets this long

PauseSetFn g_pauseSet = nullptr;
RenderToggleFn g_renderToggle = nullptr;
PlayFn g_origPlay = nullptr;
KeyHandlerFn g_origKeyHandler = nullptr;
bool g_hidden = false;
bool g_mute = false;
bool g_s2Restored = false;

enum class Action : std::uint8_t { None, Rewind, FastForward };
enum ScanResult { ScanGo, ScanLand, ScanEnd, ScanEndCap, ScanAbort };

struct RoundTable
{
    int start[32];
    int n;
    bool lastKnown;
    int prevIntro;
    int prevF;
    int lastEdge;
};

struct S2State
{
    std::uintptr_t sess = 0;
    std::uint8_t resumeSub = 3;
    bool lastPaused = false;
    bool seekUsed = false;
    bool mgrChecked = false;
    bool keysArmed = false;
    bool keysInTakeover = false;
    bool endLogged = false;
    bool prePressPaused = false;
    bool rewindFailed = false;
    bool keysMasked = false;
    bool pausedGuard = false;
    bool triggerSaved = false;
    std::uint32_t pausedTrigger = 0;
    bool seekDirty = false;
    Action action = Action::None;
    DWORD holdStart = 0;
    int lastPressed = -1;
    int takeoverFrame = -1;        // where the recording left off; -1 = not in takeover
    int takeoverKoAt = -1;
    bool takeoverPrevKo = false;
    bool takeoverBackKey = false;
    int takeoverGrace = 0;
    KeyTimer timers[kKeyCount] = {};
    RoundTable round = {{}, 0, false, 0, -1, -1};
    bool scanActive = false;
    int scanOrigin = 0;
    bool scanWasPaused = false;
    std::uint32_t scanTicks = 0;
    const char* tag = nullptr;
    DWORD tagUntil = 0;
    std::uint8_t savedKeyBindings[32] = {};
    std::uint8_t savedHiddenBindings[32] = {};
    std::uint8_t prevRender = 1;
    std::uint32_t savedMgr[3] = {};
    double lastLoadMs = 0.0;
    double lastHiddenMs = 0.0;
};
S2State g_x;

struct Pick
{
    std::uintptr_t slot;
    int frame;
    std::uint8_t sub;
    Snapshot snap;
};

template <typename T>
void Wr(std::uintptr_t address, T value)
{
    *reinterpret_cast<T*>(address) = value;
}

int Frame(std::uintptr_t s)
{
    return Rd<int>(s + kSessFrame);
}

bool Paused(std::uintptr_t s)
{
    return Rd<std::uint8_t>(s + kSessPaused) != 0;
}

bool InTakeover(std::uintptr_t s)
{
    return Rd<std::uint8_t>(s + kSessTakeover) != 0;
}

void Tag(const char* tag)
{
    g_x.tag = tag;
    g_x.tagUntil = GetTickCount() + kTagMs;
}

bool IsOurKey(int dik)
{
    for (int i = 0; i < kKeyCount; ++i)
    {
        if (replay::settings::GetRewindKey(static_cast<RewindKey>(i)) == dik)
        {
            return true;
        }
    }
    return false;
}

std::uintptr_t CurrentSys()
{
    return Rd<std::uintptr_t>(g_base + g_prof->ctxRva + g_prof->ctxSysOff);
}

bool IsReplayPlayback(std::uintptr_t sys)
{
    const auto screen0 = Rd<std::uintptr_t>(kVaScreen0);
    return Rd<std::uint32_t>(kVaScreenIndex) == 3 && screen0 != 0 && sys != 0
        && Rd<std::uint8_t>(screen0 + 0x43C) == 4 && Rd<std::uint8_t>(sys + kSysGameMode) == 5
        && Rd<std::uint8_t>(sys + kSysIoMode) <= 1;
}

// Replay-only by construction: this function sits in the replay session's vtable alone.
// Drops the call instead of editing Revival's config, as training-mode's hotkey gate does.
bool ShouldDropRevivalKey(std::uintptr_t s, int key)
{
    if (g_s2Disabled || s == 0 || Rd<std::uintptr_t>(s) != g_base + g_prof->vtableRva
        || !IsReplayPlayback(CurrentSys()))
    {
        return false;
    }
    if (IsOurKey(key))
    {
        return true;
    }
    const auto cfg = Rd<std::uintptr_t>(s + kSessCfg);
    if (cfg == 0)
    {
        return false;
    }
    // Revival's ring keys: no slot is saved in takeover, and its load clears takeover even from an
    // empty slot, which feeds the recording into the changed match. That is the stock desync.
    if (InTakeover(s) && (key == Rd<int>(cfg + 0xA0 + g_prof->cfgShift) || key == Rd<int>(cfg + 0xA4 + g_prof->cfgShift)))
    {
        g_x.takeoverBackKey = true;
        return true;
    }
    // Revival's step always goes, wherever it is bound: its path forces the sub-step count to 3.
    return key == Rd<int>(cfg + 0x110 + g_prof->cfgShift);
}

std::uintptr_t __fastcall HookedKeyHandler(void* sess, void* edx, int key)
{
    return ShouldDropRevivalKey(reinterpret_cast<std::uintptr_t>(sess), key) ? 0 : g_origKeyHandler(sess, edx, key);
}

char __fastcall HookedPlay(void* sess, void* edx, unsigned id, int f)
{
    return (g_hidden || g_mute) ? 0 : g_origPlay(sess, edx, id, f);
}

// A key of ours may also be bound as a game key: without this, pressing it could start takeover.
void MaskGameKeys(std::uintptr_t s)
{
    // A tick with the exit flag never reaches the takeover check, and it swaps a switched side's
    // bindings back: restoring a copy from before that would leave P1 and P2 swapped for good.
    if (g_x.keysMasked || InTakeover(s) || Rd<std::uint8_t>(g_live.battle + kBattleExit) != 0)
    {
        return;
    }
    auto* words = reinterpret_cast<std::uint16_t*>(kVaInputMgr + kMgrKeyBindings);
    std::memcpy(g_x.savedKeyBindings, words, sizeof(g_x.savedKeyBindings));
    const int graceSide = g_x.takeoverGrace > 0 ? static_cast<int>(Rd<std::uint32_t>(s + kSessSide) & 1u) : -1;
    for (int i = 0; i < 16; ++i)
    {
        if (i / 8 == graceSide || ((words[i] & 0xF00) == 0 && IsOurKey(words[i] & 0xFF)))
        {
            words[i] = 0x0300; // produces no input bit
        }
    }
    g_x.keysMasked = true;
}

void UnmaskGameKeys()
{
    if (g_x.keysMasked)
    {
        std::memcpy(reinterpret_cast<void*>(kVaInputMgr + kMgrKeyBindings), g_x.savedKeyBindings, sizeof(g_x.savedKeyBindings));
        g_x.keysMasked = false;
    }
}

void ObserveRound(std::uintptr_t s, int f)
{
    RoundTable& r = g_x.round;
    if (f == r.prevF)
    {
        return;
    }
    // The only writer of 0 here is the intro's end, so one edge per round, and a paused tick
    // cannot cause it. Round number and win counts change at the KO instead.
    const int intro = Rd<std::uint8_t>(g_live.sys + kSysRoundIntro);
    if (r.prevF + 1 == f && r.prevIntro != 0 && intro == 0 && !InTakeover(s))
    {
        r.lastEdge = f;
        if (r.n == 0 || f > r.start[r.n - 1])
        {
            if (r.n < 32)
            {
                r.start[r.n++] = f;
                mod::Log("RewindS2: round start[%d]=%d", r.n - 1, f);
            }
        }
        else
        {
            bool known = false;
            for (int i = 0; i < r.n; ++i)
            {
                known = known || r.start[i] == f;
            }
            if (!known)
            {
                mod::Log("RewindS2: round edge mismatch F=%d", f);
            }
        }
    }
    r.prevIntro = intro;
    r.prevF = f;
}

void OrigTick(std::uintptr_t s, void* edx)
{
    ObserveRound(s, Frame(s));
    g_origTick(reinterpret_cast<void*>(s), edx);
    const auto sub = Rd<std::uint8_t>(g_live.battle + kBattleSubSteps);
    if (!Paused(s) && sub != 0)
    {
        g_x.resumeSub = sub;
    }
}

// Paused ticks are not inert: a pending slow-motion trigger runs sub-steps, and %64 frames
// re-save the ring slot with the paused state.
void PausedTick(std::uintptr_t s, void* edx)
{
    ObserveRound(s, Frame(s)); // before the temporary takeover flag hides a round edge
    g_x.pausedTrigger = Rd<std::uint32_t>(g_live.sys + kSysSlowTrigger);
    g_x.triggerSaved = true;
    Wr<std::uint32_t>(g_live.sys + kSysSlowTrigger, 0);
    g_x.pausedGuard = (Frame(s) % 64 == 0) && !InTakeover(s);
    if (g_x.pausedGuard)
    {
        Wr<std::uint8_t>(s + kSessTakeover, 1);
    }
    OrigTick(s, edx);
    if (g_x.pausedGuard)
    {
        Wr<std::uint8_t>(s + kSessTakeover, 0);
        g_x.pausedGuard = false;
    }
    Wr<std::uint32_t>(g_live.sys + kSysSlowTrigger, g_x.pausedTrigger);
    g_x.triggerSaved = false;
}

void VisibleTick(std::uintptr_t s, void* edx)
{
    MaskGameKeys(s);
    if (Paused(s))
    {
        PausedTick(s, edx);
    }
    else
    {
        OrigTick(s, edx);
    }
    UnmaskGameKeys();
}

void MaskedPausedTick(std::uintptr_t s, void* edx)
{
    MaskGameKeys(s);
    PausedTick(s, edx);
    UnmaskGameKeys();
}

void FlipParity()
{
    const auto parity = Rd<std::uint32_t>(g_live.sys + kSysParity);
    Wr<std::uint32_t>(g_live.sys + kSysParity, parity == 0 ? 1u : 0u);
}

// Hidden ticks read live input three ways (takeover, F-key bits, Esc and F-keys in the battle
// hotkeys); all go through the keyboard device, so swap in one that reports nothing. Replay
// inputs are injected after the poll and are unaffected.
HRESULT __stdcall ZeroAcquire(void*)
{
    return S_OK;
}

HRESULT __stdcall ZeroState(void*, DWORD size, void* out)
{
    std::memset(out, 0, size);
    return S_OK;
}

void* const g_zeroVtable[10] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    reinterpret_cast<void*>(&ZeroAcquire), nullptr, reinterpret_cast<void*>(&ZeroState)};
const void* g_zeroDevice = g_zeroVtable;

// Never entered while paused: renderToggle(1) also restores the pause's NOP list.
void EnterHidden()
{
    g_hidden = true;
    for (int i = 0; i < 3; ++i)
    {
        g_x.savedMgr[i] = Rd<std::uint32_t>(kVaInputMgr + kMgrKeyboard + 4 * i);
    }
    Wr<const void*>(kVaInputMgr + kMgrKeyboard, &g_zeroDevice);
    Wr<std::uint32_t>(kVaInputMgr + kMgrKeyboard + 4, 0);
    Wr<std::uint32_t>(kVaInputMgr + kMgrKeyboard + 8, 0);
    // The zero device covers the keyboard. Without this, a held pad button takes over mid-seek.
    auto* words = reinterpret_cast<std::uint16_t*>(kVaInputMgr + kMgrKeyBindings);
    std::memcpy(g_x.savedHiddenBindings, words, sizeof(g_x.savedHiddenBindings));
    for (int i = 0; i < 16; ++i)
    {
        words[i] = 0x0300; // produces no input bit
    }
    const std::uintptr_t ctx = g_base + g_prof->ctxRva;
    g_x.prevRender = Rd<std::uint8_t>(ctx + 0x15);
    g_renderToggle(reinterpret_cast<void*>(ctx), nullptr, 0);
}

void LeaveHidden()
{
    const std::uintptr_t ctx = g_base + g_prof->ctxRva;
    g_renderToggle(reinterpret_cast<void*>(ctx), nullptr, g_x.prevRender);
    for (int i = 0; i < 3; ++i)
    {
        Wr<std::uint32_t>(kVaInputMgr + kMgrKeyboard + 4 * i, g_x.savedMgr[i]);
    }
    std::memcpy(reinterpret_cast<void*>(kVaInputMgr + kMgrKeyBindings), g_x.savedHiddenBindings, sizeof(g_x.savedHiddenBindings));
    g_hidden = false;
}

// Checked before each hidden tick. The reader's EOF flag must stay for a visible tick: the
// tick that consumes it sets the exit flag, and folder chaining has to see that.
bool ExitPending(std::uintptr_t s)
{
    return Rd<std::uint8_t>(g_live.battle + kBattleExit) != 0 || Rd<std::uint8_t>(s + kSessQuit) != 0
        || Rd<std::uint32_t>(g_live.sys + kSysReader) != 0;
}

bool HiddenStop(std::uintptr_t s)
{
    return Rd<std::uint32_t>(kVaScreenIndex) != 3 || Rd<std::uint8_t>(g_live.battle + kBattleFadeIn) != 0
        || InTakeover(s) || ExitPending(s);
}

const char* SeekRefusal()
{
    if (Rd<std::uint32_t>(g_live.battle + kBattleF8Pause) != 0) return "F8 ON";
    if (Rd<std::uint32_t>(kVaDirectDraw) == 0 || Rd<std::uint32_t>(kVaDirectDraw2) == 0) return "NO CKPT";
    return nullptr;
}

// Slots validate themselves, so no metadata can go stale: Revival's reset clears them and an
// overwritten slot shows a different frame. wantFrame -1 accepts any frame near the target.
bool SlotUsable(std::uintptr_t slot, int wantFrame, int target, Pick* out)
{
    if (Rd<std::uintptr_t>(slot) == 0)
    {
        return false;
    }
    const SlotExtras extras = ReadExtrasAt(slot);
    if (extras.used > extras.cap || !CollectAt(slot, out->snap) || out->snap.count < 7 || out->snap.count > 10)
    {
        return false;
    }
    const Snapshot& snap = out->snap;
    for (int i = 0; i < snap.count; ++i)
    {
        if (snap.recs[i].cls == RecClass::Unknown)
        {
            return false;
        }
    }
    const Rec& last = snap.recs[snap.count - 1];
    if (last.cls != RecClass::Frame || last.size != 4)
    {
        return false;
    }
    const int frame = *reinterpret_cast<const int*>(last.data);
    if (wantFrame >= 0 ? frame != wantFrame : (frame > target || target - frame > kMaxHidden))
    {
        return false;
    }
    const Rec* battle = Find(snap, RecClass::Battle);
    const Rec* battle440 = Find(snap, RecClass::Battle440);
    const Rec* sys = Find(snap, RecClass::Sys);
    if (battle == nullptr || battle440 == nullptr || sys == nullptr || sys->dst != g_live.sys
        || Find(snap, RecClass::First) == nullptr || Find(snap, RecClass::Char1) == nullptr
        || Find(snap, RecClass::Char2) == nullptr || battle440->size <= 0x14B)
    {
        return false;
    }
    if (battle->data[kBattleFadeIn] != 0 || battle->data[kBattleExit] != 0)
    {
        return false;
    }
    if (*reinterpret_cast<const std::uint32_t*>(battle440->data + (kBattleF8Pause - 0x440)) != 0)
    {
        return false; // saved during F8: it would land frozen with the label still running
    }
    const std::uint8_t sub = battle440->data[kBattleSubSteps - 0x440];
    if (sub < 1 || sub > 3)
    {
        return false; // 0 = saved while paused
    }
    out->slot = slot;
    out->frame = frame;
    out->sub = sub;
    return true;
}

bool PickSlot(std::uintptr_t s, int target, Pick* out)
{
    const auto cap = Rd<std::uint32_t>(s + kSessRingCap);
    const auto ring = Rd<std::uintptr_t>(s + kSessRing);
    if (cap != 0 && ring != 0)
    {
        // Ring[0] is never used: it is the only slot saved before the fade-in ran.
        for (int c = target & ~63; c > 0 && target - c <= kMaxHidden; c -= 64)
        {
            if (SlotUsable(ring + kSlotSize * ((c / 64) % cap), c, target, out))
            {
                return true;
            }
        }
    }
    return SlotUsable(s + kSessManualSlot, -1, target, out); // frame 1, unless Revival's Save was pressed
}

// The palette's dst comes from a wrapper call that may move on a device reset [inferred].
bool PaletteMoved(std::uintptr_t s, const Pick& pick)
{
    const Rec* palette = Find(pick.snap, RecClass::Palette);
    const int newest = Frame(s) & ~63;
    const auto cap = Rd<std::uint32_t>(s + kSessRingCap);
    const auto ring = Rd<std::uintptr_t>(s + kSessRing);
    if (palette == nullptr || newest <= 0 || cap == 0 || ring == 0)
    {
        return false;
    }
    Pick now = {};
    if (!SlotUsable(ring + kSlotSize * ((newest / 64) % cap), newest, newest, &now))
    {
        return false;
    }
    const Rec* current = Find(now.snap, RecClass::Palette);
    if (current == nullptr || current->dst == palette->dst)
    {
        return false;
    }
    mod::LogOnce("RewindS2.paletteMoved", "RewindS2: palette dst moved");
    return true;
}

void LoadFcw(std::uint16_t fcw)
{
    __asm fldcw fcw
}

// False = nothing touched.
bool SeekCore(std::uintptr_t s, void* edx, int target, bool endPaused)
{
    // A paused tick at frame 1 would overwrite the manual slot: its save is not gated.
    target = (std::max)(target, endPaused ? 2 : 1);
    Pick pick = {};
    if (!PickSlot(s, target, &pick))
    {
        mod::LogThrottled("RewindS2.noCkpt", 1000, "RewindS2: no ckpt target=%d", target);
        return false;
    }
    const bool paletteMoved = PaletteMoved(s, pick);
    g_x.seekUsed = true;
    if (InTakeover(s))
    {
        g_x.takeoverGrace = kTakeoverGraceTicks; // the load below ends it
    }
    if (Paused(s))
    {
        g_pauseSet(reinterpret_cast<void*>(s), nullptr, 0); // never the toggler: this also sets the read gate
    }

    // Node patches for the load only: a later re-save frees these nodes.
    const bool stage10 = IsStageType10();
    std::uintptr_t savedDst[kMaxRecs] = {};
    std::uint32_t savedSize[kMaxRecs] = {};
    const Snapshot& snap = pick.snap;
    for (int i = 0; i < snap.count; ++i)
    {
        const Rec& rec = snap.recs[i];
        savedDst[i] = rec.dst;
        savedSize[i] = rec.size;
        if (rec.cls == RecClass::Surface || (rec.cls == RecClass::Palette && paletteMoved))
        {
            Wr<std::uintptr_t>(rec.node + 8, reinterpret_cast<std::uintptr_t>(rec.data)); // copies onto itself
        }
        else if (rec.cls == RecClass::Stage && stage10 && rec.size > 0x70)
        {
            Wr<std::uint32_t>(rec.node + 16, 0x70);
        }
    }
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    g_x.seekDirty = true;
    g_replayLoad(reinterpret_cast<void*>(s), nullptr, reinterpret_cast<void*>(pick.slot));
    g_x.lastLoadMs = MsSince(start);
    for (int i = 0; i < snap.count; ++i)
    {
        Wr<std::uintptr_t>(snap.recs[i].node + 8, savedDst[i]);
        Wr<std::uint32_t>(snap.recs[i].node + 16, savedSize[i]);
    }

    g_x.round.prevF = -1;
    if (Frame(s) != pick.frame)
    {
        Disable("frame after load");
        return true;
    }
    Wr<std::uint8_t>(g_live.battle + kBattleSubSteps, pick.sub); // the load keeps the live value
    const auto fcw = Rd<std::uint16_t>(Rd<std::uintptr_t>(pick.slot));
    LoadFcw(fcw); // the load does not restore it

    QueryPerformanceCounter(&start);
    if (Frame(s) < target)
    {
        EnterHidden();
        bool first = true;
        for (int n = 0; Frame(s) < target && n <= kMaxHidden && !HiddenStop(s); ++n)
        {
            if (!first)
            {
                FlipParity(); // main-loop work a hidden tick skips
            }
            OrigTick(s, edx);
            first = false;
        }
        LeaveHidden();
        if (!first)
        {
            FlipParity();
        }
    }
    g_x.lastHiddenMs = MsSince(start);
    g_x.seekDirty = false;

    const bool slow = g_x.lastLoadMs + g_x.lastHiddenMs > 50.0;
    const char* const format = "RewindS2: seek F=%d C=%d K=%d paused=%d load=%.3fms hidden=%.3fms";
    if (slow)
    {
        mod::Log(format, Frame(s), pick.frame, target - pick.frame, endPaused ? 1 : 0, g_x.lastLoadMs, g_x.lastHiddenMs);
    }
    else
    {
        mod::LogThrottled("RewindS2.seek", 1000, format, Frame(s), pick.frame, target - pick.frame, endPaused ? 1 : 0,
            g_x.lastLoadMs, g_x.lastHiddenMs);
    }
    return true;
}

bool Seek(std::uintptr_t s, void* edx, int target, bool endPaused)
{
    if (!SeekCore(s, edx, target, endPaused))
    {
        return false;
    }
    if (endPaused && !ExitPending(s))
    {
        g_x.resumeSub = Rd<std::uint8_t>(g_live.battle + kBattleSubSteps);
        g_pauseSet(reinterpret_cast<void*>(s), nullptr, 1);
        MaskedPausedTick(s, edx);
    }
    else
    {
        VisibleTick(s, edx);
    }
    return true;
}

void ForcePause(std::uintptr_t s)
{
    g_x.resumeSub = Rd<std::uint8_t>(g_live.battle + kBattleSubSteps);
    g_pauseSet(reinterpret_cast<void*>(s), nullptr, 1);
}

// Revival's unpause forces the sub-step count to 3: put the real one back. After a seek,
// also discard what paused ticks polluted; before the first seek, stay as close to stock as we can.
void CleanResume(std::uintptr_t s, void* edx)
{
    if (!g_x.seekUsed || InTakeover(s) || SeekRefusal() != nullptr || !SeekCore(s, edx, Frame(s), false))
    {
        Wr<std::uint8_t>(g_live.battle + kBattleSubSteps, g_x.resumeSub);
    }
}

void Unpause(std::uintptr_t s, void* edx)
{
    if (Paused(s))
    {
        g_pauseSet(reinterpret_cast<void*>(s), nullptr, 0);
    }
    CleanResume(s, edx);
}

constexpr int HoldSpeed(DWORD heldMs)
{
    const DWORD level = heldMs / kHoldRampMs;
    return level >= 3 ? 8 : 1 << level;
}

static_assert(HoldSpeed(0) == 1 && HoldSpeed(499) == 1 && HoldSpeed(500) == 2 && HoldSpeed(1000) == 4);
static_assert(HoldSpeed(1500) == 8 && HoldSpeed(60000) == 8);

bool TrySeekBack(std::uintptr_t s, void* edx, int frames)
{
    const int f = Frame(s);
    if (f < 3)
    {
        Tag("NO CKPT");
        return false;
    }
    if (const char* refusal = SeekRefusal())
    {
        Tag(refusal);
        return false;
    }
    // A fast step can overshoot the oldest checkpoint: a refused seek changes nothing, so get as close as possible.
    for (; frames > 0; frames /= 2)
    {
        if (Seek(s, edx, (std::max)(f - frames, 2), true))
        {
            return true;
        }
    }
    Tag("NO CKPT");
    return false;
}

void StepBack(std::uintptr_t s, void* edx)
{
    const bool wasPaused = Paused(s);
    if (!TrySeekBack(s, edx, 1))
    {
        if (!wasPaused && !InTakeover(s))
        {
            ForcePause(s);
        }
        VisibleTick(s, edx);
    }
}

void StepForward(std::uintptr_t s, void* edx)
{
    if (!Paused(s))
    {
        VisibleTick(s, edx);
        if (!ExitPending(s))
        {
            ForcePause(s);
        }
        return;
    }
    if (!InTakeover(s) && SeekRefusal() == nullptr && Seek(s, edx, Frame(s) + 1, true))
    {
        return;
    }
    g_pauseSet(reinterpret_cast<void*>(s), nullptr, 0);
    Wr<std::uint8_t>(g_live.battle + kBattleSubSteps, g_x.resumeSub);
    MaskGameKeys(s);
    OrigTick(s, edx);
    UnmaskGameKeys();
    if (!ExitPending(s))
    {
        ForcePause(s);
    }
}

void FastForwardBatch(std::uintptr_t s, void* edx)
{
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    EnterHidden();
    bool first = true;
    // 2x to 16x: the hidden batch plus 1 visible tick, since visible ticks cannot beat vsync.
    const int hidden = 2 * HoldSpeed(GetTickCount() - g_x.holdStart) - 1;
    for (int n = 0; n < hidden && MsSince(start) < kFfBudgetMs && !HiddenStop(s); ++n)
    {
        if (!first)
        {
            FlipParity();
        }
        OrigTick(s, edx);
        first = false;
    }
    LeaveHidden();
    if (!first)
    {
        FlipParity();
    }
    VisibleTick(s, edx);
}

constexpr int PrevRoundTarget(const int* start, int n, int f)
{
    int cur = n - 1;
    while (cur >= 0 && start[cur] > f)
    {
        --cur;
    }
    if (cur < 0)
    {
        return -1;
    }
    const bool justStarted = f - start[cur] < kPrevRoundGrace;
    if (justStarted && cur > 0)
    {
        return start[cur - 1];
    }
    return start[cur];
}

constexpr int kRoundStarts[] = {100, 1000, 2000};
static_assert(PrevRoundTarget(kRoundStarts, 3, 50) == -1);
static_assert(PrevRoundTarget(kRoundStarts, 3, 100) == 100);
static_assert(PrevRoundTarget(kRoundStarts, 3, 500) == 100);
static_assert(PrevRoundTarget(kRoundStarts, 3, 1500) == 1000);
static_assert(PrevRoundTarget(kRoundStarts, 3, 1990) == 1000);
static_assert(PrevRoundTarget(kRoundStarts, 3, 1000) == 100);
static_assert(PrevRoundTarget(kRoundStarts, 3, 1059) == 100);
static_assert(PrevRoundTarget(kRoundStarts, 3, 1060) == 1000);

bool PrevRound(std::uintptr_t s, void* edx)
{
    if (const char* refusal = SeekRefusal())
    {
        Tag(refusal);
        return false;
    }
    const RoundTable& r = g_x.round;
    const int f = Frame(s);
    // In takeover the frame count no longer means anything: the round is the recording's.
    const bool takeover = InTakeover(s);
    int target = PrevRoundTarget(r.start, r.n, takeover ? g_x.takeoverFrame + kPrevRoundGrace : f);
    if (takeover && (target < 0 || target > g_x.takeoverFrame))
    {
        target = g_x.takeoverFrame;
    }
    if (target < 0 || (target == f && !takeover))
    {
        Tag("ROUND 1");
        return false;
    }
    if (!Seek(s, edx, target, Paused(s)))
    {
        Tag("NO CKPT");
        return false;
    }
    return true;
}

ScanResult ScanStop(std::uintptr_t s)
{
    const int f = Frame(s);
    ObserveRound(s, f);
    if (g_x.round.lastEdge > g_x.scanOrigin) return ScanLand;
    if (InTakeover(s) || Rd<std::uint32_t>(g_live.battle + kBattleF8Pause) != 0) return ScanAbort;
    const auto cap = Rd<std::uint32_t>(s + kSessRingCap);
    if (cap != 0 && f - g_x.scanOrigin >= static_cast<int>(cap - 1) * 64) return ScanEndCap; // keeps the origin's checkpoint
    if (Rd<std::uint8_t>(g_live.battle + kBattleFadeIn) != 0 || ExitPending(s)) return ScanEnd;
    return ScanGo;
}

void FinishScan(ScanResult result, std::uintptr_t s, void* edx)
{
    g_x.scanActive = false;
    mod::Log("RewindS2: scan origin=%d end=%d F=%d ticks=%u", g_x.scanOrigin, static_cast<int>(result), Frame(s), g_x.scanTicks);
    if (result == ScanLand)
    {
        if (g_x.scanWasPaused)
        {
            ForcePause(s);
            MaskedPausedTick(s, edx);
        }
        else
        {
            VisibleTick(s, edx);
        }
        return;
    }
    if (result == ScanEnd)
    {
        g_x.round.lastKnown = true;
    }
    Wr<std::uint8_t>(s + kSessQuit, 0); // not in the slot; the exit flag and reader state are
    if (Seek(s, edx, g_x.scanOrigin, g_x.scanWasPaused))
    {
        if (result == ScanEnd) Tag("LAST ROUND");
        if (result == ScanEndCap) Tag("NO CKPT");
    }
    else
    {
        mod::Log("RewindS2: scan origin slot gone");
        VisibleTick(s, edx);
    }
}

void ScanIteration(std::uintptr_t s, void* edx, const std::uint8_t (&keys)[256], const KeyEvent (&ev)[kKeyCount])
{
    if ((keys[1] & 0x80) != 0)
    {
        g_x.scanActive = false;
        VisibleTick(s, edx);
        return;
    }
    bool anyPress = false;
    for (int i = 0; i < kKeyCount; ++i)
    {
        anyPress = anyPress || ev[i] == KeyEvent::Press;
    }
    if (anyPress || Paused(s))
    {
        FinishScan(ScanAbort, s, edx);
        return;
    }
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    EnterHidden();
    bool first = true;
    ScanResult result = ScanGo;
    for (int n = 0; n < kScanTicks && MsSince(start) < kScanBudgetMs; ++n)
    {
        result = ScanStop(s);
        if (result != ScanGo)
        {
            break;
        }
        if (!first)
        {
            FlipParity();
        }
        OrigTick(s, edx);
        ++g_x.scanTicks;
        first = false;
    }
    LeaveHidden();
    if (result == ScanGo)
    {
        result = ScanStop(s);
    }
    if (!first)
    {
        FlipParity();
    }
    if (result != ScanGo)
    {
        FinishScan(result, s, edx);
        return;
    }
    g_mute = true;
    VisibleTick(s, edx);
    g_mute = false;
}

bool NextRound(std::uintptr_t s, void* edx, const std::uint8_t (&keys)[256])
{
    if (const char* refusal = SeekRefusal())
    {
        Tag(refusal);
        return false;
    }
    const bool paused = Paused(s);
    if (InTakeover(s) && !SeekCore(s, edx, g_x.takeoverFrame, false)) // "next" counts from the recording
    {
        Tag("NO CKPT");
        return false;
    }
    g_x.takeoverFrame = -1;
    RoundTable& r = g_x.round;
    const int f = Frame(s);
    int target = 0;
    for (int i = r.n - 1; i >= 0; --i)
    {
        if (r.start[i] > f)
        {
            target = r.start[i];
        }
    }
    Pick pick = {};
    if (target != 0 && PickSlot(s, target, &pick))
    {
        return Seek(s, edx, target, paused);
    }
    if (target == 0 && r.lastKnown)
    {
        Tag("LAST ROUND");
        if (paused && !Paused(s))
        {
            ForcePause(s);
        }
        return false;
    }
    if (!PickSlot(s, f, &pick))
    {
        Tag("NO CKPT");
        if (paused && !Paused(s))
        {
            ForcePause(s);
        }
        return false;
    }
    r.lastEdge = -1;
    g_x.scanActive = true;
    g_x.scanOrigin = f;
    g_x.scanWasPaused = paused;
    g_x.scanTicks = 0;
    if (paused)
    {
        Unpause(s, edx);
    }
    const KeyEvent none[kKeyCount] = {}; // this press started the scan; it must not cancel it
    ScanIteration(s, edx, keys, none);
    return true;
}

// Always ends in exactly one visible tick.
void Dispatch(std::uintptr_t s, void* edx, const std::uint8_t (&keys)[256], const KeyEvent (&ev)[kKeyCount])
{
    constexpr int kBack = static_cast<int>(RewindKey::Back);
    constexpr int kForward = static_cast<int>(RewindKey::Forward);
    constexpr int kPrev = static_cast<int>(RewindKey::PrevRound);
    constexpr int kNext = static_cast<int>(RewindKey::NextRound);
    const bool paused = Paused(s);
    const bool takeover = InTakeover(s);

    if (ev[kBack] == KeyEvent::Press && !takeover) // TakeoverStep owns it in takeover
    {
        g_x.action = Action::None;
        g_x.prePressPaused = paused;
        StepBack(s, edx);
        return;
    }
    if (ev[kForward] == KeyEvent::Press)
    {
        g_x.action = Action::None;
        g_x.prePressPaused = paused;
        StepForward(s, edx);
        return;
    }
    if (ev[kPrev] == KeyEvent::Press)
    {
        g_x.action = Action::None;
        if (PrevRound(s, edx))
        {
            return;
        }
    }
    if (ev[kNext] == KeyEvent::Press)
    {
        g_x.action = Action::None;
        if (NextRound(s, edx, keys))
        {
            return;
        }
    }

    if (ev[kBack] == KeyEvent::HoldStart && g_x.lastPressed == kBack && !takeover)
    {
        g_x.action = Action::Rewind;
        g_x.holdStart = GetTickCount();
        g_x.rewindFailed = false;
    }
    if (ev[kForward] == KeyEvent::HoldStart && g_x.lastPressed == kForward && !takeover)
    {
        g_x.action = Action::FastForward;
        g_x.holdStart = GetTickCount();
        if (paused)
        {
            Unpause(s, edx);
        }
    }
    if (ev[kBack] == KeyEvent::HoldEnd && g_x.action == Action::Rewind)
    {
        g_x.action = Action::None;
        if (!g_x.prePressPaused && !ExitPending(s))
        {
            Unpause(s, edx);
        }
    }
    if (ev[kForward] == KeyEvent::HoldEnd && g_x.action == Action::FastForward)
    {
        g_x.action = Action::None;
        if (g_x.prePressPaused && !Paused(s) && !ExitPending(s))
        {
            ForcePause(s);
        }
    }

    if (g_x.action == Action::Rewind && !g_x.rewindFailed)
    {
        if (TrySeekBack(s, edx, HoldSpeed(GetTickCount() - g_x.holdStart)))
        {
            return;
        }
        g_x.rewindFailed = true;
    }
    if (g_x.action == Action::FastForward)
    {
        if (Paused(s))
        {
            g_x.action = Action::None;
        }
        else
        {
            FastForwardBatch(s, edx);
            return;
        }
    }
    VisibleTick(s, edx);
}

void ResetForNewReplay(std::uintptr_t s)
{
    g_x = S2State{};
    g_x.sess = s;
}

void LogEndOnce(std::uintptr_t s)
{
    if (g_x.endLogged || Rd<std::uint8_t>(g_live.battle + kBattleExit) == 0 || InTakeover(s))
    {
        return;
    }
    g_x.endLogged = true;
    // Same replay, played twice, one with seeks: these lines must match.
    mod::Log("RewindS2: end F=%d exit=%u wins=%d/%d c264=%u/%u rounds=%d", Frame(s),
        Rd<std::uint8_t>(g_live.battle + kBattleExit), Rd<std::int8_t>(g_live.char1 + 0xAC),
        Rd<std::int8_t>(g_live.char2 + 0xAC), Rd<std::uint32_t>(g_live.char1 + 264),
        Rd<std::uint32_t>(g_live.char2 + 264), g_x.round.n);
}

void StepAllKeys(std::uintptr_t s, const std::uint8_t (&keys)[256], KeyEvent (&ev)[kKeyCount])
{
    const DWORD now = GetTickCount();
    const bool takeover = InTakeover(s);
    const bool takeoverEnded = g_x.keysInTakeover && !takeover;
    g_x.keysInTakeover = takeover;
    const auto* sideWords = reinterpret_cast<const std::uint16_t*>(
        kVaInputMgr + kMgrKeyBindings + 16 * (Rd<std::uint32_t>(s + kSessSide) & 1u));
    for (int i = 0; i < kKeyCount; ++i)
    {
        const int dik = replay::settings::GetRewindKey(static_cast<RewindKey>(i));
        bool down = (keys[dik] & 0x80) != 0;
        bool played = false;
        for (int w = 0; w < 8; ++w)
        {
            played = played || ((sideWords[w] & 0xF00) == 0 && (sideWords[w] & 0xFF) == dik);
        }
        if (takeover && played)
        {
            down = false; // takeover play comes first: a key the active side plays with is not a rewind key
        }
        if (!g_x.keysArmed || (takeoverEnded && played)) // still held from play: not a press
        {
            replay::input::ArmKey(g_x.timers[i], down);
            ev[i] = KeyEvent::None;
            continue;
        }
        ev[i] = replay::input::StepKey(g_x.timers[i], down, now, kHoldMs);
        if (ev[i] == KeyEvent::Press)
        {
            g_x.lastPressed = i;
        }
    }
    g_x.keysArmed = true;
}

// Revival can only leave takeover by loading a save, so we do it for the player: back to the
// frame the recording left off, then it plays on. True = this ran the tick.
bool TakeoverStep(std::uintptr_t s, void* edx, const std::uint8_t (&keys)[256], const KeyEvent (&ev)[kKeyCount])
{
    if (!InTakeover(s))
    {
        g_x.takeoverFrame = -1;
        g_x.takeoverBackKey = false;
        if (g_x.takeoverGrace > 0)
        {
            const auto* words = reinterpret_cast<const std::uint16_t*>(
                kVaInputMgr + kMgrKeyBindings + 16 * (Rd<std::uint32_t>(s + kSessSide) & 1u));
            bool held = false;
            for (int w = 0; w < 8; ++w)
            {
                held = held || ((words[w] & 0xF00) == 0 && (keys[words[w] & 0xFF] & 0x80) != 0);
            }
            if (!held)
            {
                --g_x.takeoverGrace;
            }
        }
        return false;
    }
    const int f = Frame(s);
    const bool ko = Rd<std::uint32_t>(g_live.sys + kSysKo) != 0;
    if (g_x.takeoverFrame < 0)
    {
        g_x.takeoverFrame = Paused(s) ? f : f - 1; // the tick that started it has already advanced
        g_x.takeoverKoAt = -1;
        g_x.takeoverPrevKo = ko;
        if (g_x.action == Action::Rewind && !g_x.prePressPaused)
        {
            Unpause(s, edx);
        }
        g_x.action = Action::None;
        mod::Log("RewindS2: takeover start F=%d side=%u", g_x.takeoverFrame, Rd<std::uint32_t>(s + kSessSide));
    }
    if (ko && !g_x.takeoverPrevKo)
    {
        g_x.takeoverKoAt = f;
    }
    g_x.takeoverPrevKo = ko;

    const auto exit = Rd<std::uint8_t>(g_live.battle + kBattleExit);
    if (exit == 1 || Rd<std::uint8_t>(g_live.battle + kBattleFadeIn) != 0)
    {
        return false;
    }
    const bool pressed = ev[static_cast<int>(RewindKey::Back)] == KeyEvent::Press || g_x.takeoverBackKey;
    g_x.takeoverBackKey = false;
    const bool roundOver = g_x.takeoverKoAt >= 0 && (!ko || f - g_x.takeoverKoAt >= kTakeoverKoFrames);
    const bool outOfInputs = Rd<std::uint8_t>(s + kSessQuit) != 0;
    if (!pressed && !roundOver && !outOfInputs && exit != 2)
    {
        return false;
    }
    if (const char* refusal = SeekRefusal())
    {
        Tag(refusal);
        return false;
    }
    Wr<std::uint8_t>(s + kSessQuit, 0); // not in the slot, and a hidden tick would stop on it
    if (!Seek(s, edx, g_x.takeoverFrame, false))
    {
        Wr<std::uint8_t>(s + kSessQuit, outOfInputs ? 1 : 0);
        g_x.takeoverKoAt = -1;
        Tag("NO CKPT");
        return false;
    }
    mod::Log("RewindS2: takeover end from=%d to=%d key=%d ko=%d out=%d exit=%u", f, g_x.takeoverFrame, pressed ? 1 : 0,
        roundOver ? 1 : 0, outOfInputs ? 1 : 0, exit);
    g_x.takeoverFrame = -1;
    g_x.prePressPaused = false;
    Tag("REPLAY");
    return true;
}

// True = not replay playback: the caller runs the original outside the fault guard, so a crash
// on any other screen behaves exactly as it would without this mod.
bool TickMain(std::uintptr_t s, void* edx)
{
    if (s == 0 || Rd<std::uintptr_t>(s) != g_base + g_prof->vtableRva)
    {
        return true;
    }
    const auto screen = Rd<std::uint32_t>(kVaScreenIndex);
    if (screen != 3 || Rd<std::uint32_t>(s + 0x60) != screen || s != g_x.sess)
    {
        ResetForNewReplay(s); // the tick's own reset runs on this same call
        return true;
    }
    if (RefreshLive(s) != nullptr || !IsReplayPlayback(g_live.sys))
    {
        return true;
    }
    if (!g_x.mgrChecked)
    {
        g_x.mgrChecked = true;
        if (Rd<std::uintptr_t>(g_live.battle + 0x24) != kVaInputMgr || Rd<std::uintptr_t>(g_live.sys + 4) != kVaInputMgr)
        {
            Disable("input manager address");
            return true;
        }
    }
    LogEndOnce(s);

    std::uint8_t keys[256];
    (void)replay::hooks::ReadRewindKeyboard(keys);
    KeyEvent ev[kKeyCount] = {};
    StepAllKeys(s, keys, ev);

    if (g_x.scanActive)
    {
        ScanIteration(s, edx, keys, ev);
    }
    else if (TakeoverStep(s, edx, keys, ev))
    {
    }
    else if (Rd<std::uint8_t>(g_live.battle + kBattleFadeIn) != 0 || Rd<std::uint8_t>(g_live.battle + kBattleExit) != 0
        || Rd<std::uint8_t>(s + kSessQuit) != 0)
    {
        g_x.action = Action::None;
        VisibleTick(s, edx);
    }
    else
    {
        if (g_x.lastPaused && !Paused(s))
        {
            CleanResume(s, edx);
        }
        Dispatch(s, edx, keys, ev);
    }
    g_x.lastPaused = Paused(s);
    return false;
}

// No C++ objects here either. Replay ticks run inside, so a fault still writes a dump.
bool GuardedMain(void* sess, void* edx, bool* passThrough)
{
    __try
    {
        *passThrough = TickMain(reinterpret_cast<std::uintptr_t>(sess), edx);
        return true;
    }
    __except (crash::HandleSehException("RewindS2", GetExceptionInformation()))
    {
        return false;
    }
}

void RestoreAfterFault()
{
    __try
    {
        if (g_hidden)
        {
            LeaveHidden();
        }
        UnmaskGameKeys();
        g_mute = false;
        if (g_x.pausedGuard)
        {
            Wr<std::uint8_t>(g_live.sess + kSessTakeover, 0);
        }
        if (g_x.triggerSaved)
        {
            Wr<std::uint32_t>(g_live.sys + kSysSlowTrigger, g_x.pausedTrigger);
        }
        if (g_x.seekDirty)
        {
            Wr<std::uint8_t>(g_live.battle + kBattleExit, 1);
        }
    }
    __except (crash::HandleSehException("RewindS2.restore", GetExceptionInformation()))
    {
        g_hidden = false;
        g_mute = false;
    }
}

void __fastcall HookedTick(void* sess, void* edx)
{
    if (g_s2Disabled)
    {
        g_origTick(sess, edx);
        return;
    }
    bool passThrough = false;
    if (!GuardedMain(sess, edx, &passThrough))
    {
        g_s2Disabled = true;
    }
    if (passThrough)
    {
        g_origTick(sess, edx);
    }
    if (g_s2Disabled && !g_s2Restored)
    {
        g_s2Restored = true;
        mod::Log("RewindS2: disabled after a fault; Revival's keys are stock again");
        RestoreAfterFault();
    }
}

bool Install()
{
    if (!replay::settings::GetRewindEnabled())
    {
        mod::Log("Rewind: off in the ini");
        return false;
    }
    g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA("EfzRevival.dll"));
    if (g_base == 0)
    {
        mod::Log("Rewind: disabled reason=no EfzRevival.dll");
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_base + dos->e_lfanew);
    const std::uint32_t ts = nt->FileHeader.TimeDateStamp;
    for (const RewindProfile& profile : kProfiles)
    {
        if (profile.ts == ts) g_prof = &profile;
    }
    if (g_prof == nullptr)
    {
        mod::Log("Rewind: disabled reason=unknown build ts=0x%08X", ts);
        return false;
    }
    if (nt->OptionalHeader.SizeOfImage != g_prof->sizeOfImage)
    {
        mod::Log("Rewind: disabled reason=SizeOfImage ts=0x%08X got=0x%X", ts,
            static_cast<unsigned>(nt->OptionalHeader.SizeOfImage));
        return false;
    }
    const auto vtableEntry = [](std::uint32_t slot) { return Rd<std::uintptr_t>(g_base + g_prof->vtableRva + 4 * slot); };
    const std::uintptr_t tick = g_base + g_prof->tickRva;
    if (vtableEntry(g_prof->tickSlot) != tick)
    {
        mod::Log("Rewind: disabled reason=vtable entry ts=0x%08X", ts);
        return false;
    }
    // Also refuses a function someone already detoured (entry no longer matches).
    if (!MatchSig("tick", g_prof->tickRva, g_prof->sigTick) || !MatchSig("load", g_prof->replayLoadRva, g_prof->sigLoad))
    {
        return false;
    }
    g_replayLoad = reinterpret_cast<ReplayIoFn>(g_base + g_prof->replayLoadRva);

    const std::uintptr_t play = g_base + g_prof->playRva;
    const std::uintptr_t keyHandler = g_base + g_prof->keyHandlerRva;
    if (vtableEntry(g_prof->playSlot) != play || vtableEntry(g_prof->keySlot) != keyHandler)
    {
        mod::Log("RewindS2: disabled reason=vtable entry ts=0x%08X", ts);
        return false;
    }
    if (!MatchSig("pauseSet", g_prof->pauseSetRva, g_prof->sigPause)
        || !MatchSig("renderToggle", g_prof->renderToggleRva, g_prof->sigRender)
        || !MatchSig("play", g_prof->playRva, g_prof->sigPlay)
        || !MatchSig("keyHandler", g_prof->keyHandlerRva, g_prof->sigKey)
        || !MatchSig("stepCmp", g_prof->stepCmpRva, g_prof->sigStepCmp))
    {
        return false;
    }
    g_pauseSet = reinterpret_cast<PauseSetFn>(g_base + g_prof->pauseSetRva);
    g_renderToggle = reinterpret_cast<RenderToggleFn>(g_base + g_prof->renderToggleRva);

    // A partial install is safe: g_s2Disabled makes both pass through, so Revival's keys stay stock.
    g_s2Disabled = true;
    if (!replay::hook::Install(reinterpret_cast<void*>(play), reinterpret_cast<void*>(&HookedPlay),
            reinterpret_cast<void**>(&g_origPlay), "revivalReplayPlay")
        || !replay::hook::Install(reinterpret_cast<void*>(keyHandler), reinterpret_cast<void*>(&HookedKeyHandler),
            reinterpret_cast<void**>(&g_origKeyHandler), "revivalReplayKeys")
        || !replay::hook::Install(reinterpret_cast<void*>(tick), reinterpret_cast<void*>(&HookedTick),
            reinterpret_cast<void**>(&g_origTick), "revivalReplayTick"))
    {
        mod::Log("RewindS2: disabled reason=hook install");
        return false;
    }
    g_s2Disabled = false;
    using replay::settings::GetRewindKey;
    mod::Log("RewindS2: install ver=%s tick=%p play=%p pause=%p render=%p keys=%p", g_prof->name,
        reinterpret_cast<void*>(tick), reinterpret_cast<void*>(play), reinterpret_cast<void*>(g_pauseSet),
        reinterpret_cast<void*>(g_renderToggle), reinterpret_cast<void*>(keyHandler));
    mod::Log("RewindS2: keys back=%02X fwd=%02X prev=%02X next=%02X", GetRewindKey(RewindKey::Back),
        GetRewindKey(RewindKey::Forward), GetRewindKey(RewindKey::PrevRound), GetRewindKey(RewindKey::NextRound));
    return true;
}
}

namespace replay::hooks
{
bool EnsureRewindInstalled()
{
    static bool attempted = false;
    static bool installed = false;
    if (!attempted)
    {
        attempted = true;
        installed = Install();
    }
    return installed;
}

void RemoveRewind()
{
    if (g_prof == nullptr)
    {
        return;
    }
    const std::uintptr_t targets[] = {g_base + g_prof->tickRva, g_base + g_prof->keyHandlerRva, g_base + g_prof->playRva};
    if (g_origTick != nullptr)
    {
        (void)MH_DisableHook(reinterpret_cast<void*>(targets[0]));
        g_origTick = nullptr;
    }
    if (g_origKeyHandler != nullptr)
    {
        (void)MH_DisableHook(reinterpret_cast<void*>(targets[1]));
        g_origKeyHandler = nullptr;
    }
    if (g_origPlay != nullptr)
    {
        (void)MH_DisableHook(reinterpret_cast<void*>(targets[2]));
        g_origPlay = nullptr;
    }
}

bool IsRewindAvailable()
{
    return g_origTick != nullptr && !g_s2Disabled;
}

bool IsHiddenTick()
{
    return g_hidden;
}

const char* GetRewindTag()
{
    if (g_s2Disabled)
    {
        return nullptr;
    }
    if (g_x.tag != nullptr && GetTickCount() < g_x.tagUntil)
    {
        return g_x.tag;
    }
    if (g_x.takeoverFrame >= 0)
    {
        return "TAKEOVER";
    }
    if (g_x.action == Action::Rewind && !g_x.rewindFailed)
    {
        return "REW";
    }
    if (g_x.action == Action::FastForward || g_x.scanActive)
    {
        return "FF";
    }
    return nullptr;
}

bool ReadRewindKeyboard(std::uint8_t (&keys)[256])
{
    std::memset(keys, 0, sizeof(keys));
    HWND foreground = GetForegroundWindow();
    DWORD pid = 0;
    if (foreground == nullptr || GetWindowThreadProcessId(foreground, &pid) == 0 || pid != GetCurrentProcessId())
    {
        return false;
    }
    void* device = *reinterpret_cast<void**>(kVaInputMgr + kMgrKeyboard);
    if (device == nullptr)
    {
        return false;
    }
    void** vtable = *reinterpret_cast<void***>(device);
    using AcquireFn = HRESULT(__stdcall*)(void*);
    using GetStateFn = HRESULT(__stdcall*)(void*, DWORD, void*);
    (void)reinterpret_cast<AcquireFn>(vtable[7])(device);
    if (FAILED(reinterpret_cast<GetStateFn>(vtable[9])(device, sizeof(keys), keys)))
    {
        std::memset(keys, 0, sizeof(keys));
        return false;
    }
    return true;
}

void NoteBattleFault()
{
    if (g_hidden)
    {
        g_s2Disabled = true;
    }
}
}
