#include "replay/render/d3d9_overlay.h"

#include "compat/overlay_context_scope.h"
#include "compat/overlay_reset_api.h"
#include "compat/overlay_reset_gate.h"
#include "logger.h"
#include "replay/core/hook_install.h"
#include "replay/core/menu_motion.h"
#include "replay/render/software_font.h"

#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "MinHook.h"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace replay::render::d3d9
{
namespace
{
constexpr int kItcBoltTtfResourceId = 102;
// RT pixels (the game RT is 640x480, i.e. 2x the 320x240 menu space). Tuning knob.
constexpr float kFontPx = 18.0f;
constexpr float kSmallFontPx = 14.0f;
ImFont* g_smallFont = nullptr;
constexpr std::size_t kMaxItems = 256;
// Software text resumes if EndScene stops firing (e.g. another mod unhooked us).
// Long enough to ride out the game's blocking load before a screen's first fade frame.
constexpr DWORD kEndSceneStaleMs = 1000;

using EndSceneFn = HRESULT(WINAPI*)(IDirect3DDevice9*);
EndSceneFn g_originalEndScene = nullptr;
void* g_endSceneTarget = nullptr;
using ResetFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
ResetFn g_originalReset = nullptr;
void* g_resetTarget = nullptr;
bool g_hookAttempted = false;
bool g_hookInstalled = false;
std::atomic<DWORD> g_lastEndSceneTick{0};
std::atomic<bool> g_backendFailed{false};

EfzOverlayResetGate g_resetGate;
ImGuiContext* g_context = nullptr;
DWORD g_contextThread = 0;
IDirect3DDevice9* g_device = nullptr;
bool g_deviceObjectsLive = false;

CRITICAL_SECTION g_textLock;
INIT_ONCE g_textLockOnce = INIT_ONCE_STATIC_INIT;
std::vector<TextItem> g_staging;
std::vector<TextItem> g_active;
std::map<std::string, DWORD> g_scrollStart;
std::atomic<bool> g_published{false};
BOOL CALLBACK InitTextLock(PINIT_ONCE, PVOID, PVOID*)
{
    InitializeCriticalSection(&g_textLock);
    return TRUE;
}

CRITICAL_SECTION& TextLock()
{
    InitOnceExecuteOnce(&g_textLockOnce, &InitTextLock, nullptr, nullptr);
    return g_textLock;
}

HMODULE SelfModule()
{
    HMODULE module = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&SelfModule),
        &module);
    return module;
}

bool IsStbLoadableFontFile(const char* path)
{
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    std::uint8_t magic[4] = {};
    DWORD read = 0;
    const BOOL ok = ReadFile(file, magic, sizeof(magic), &read, nullptr);
    CloseHandle(file);
    if (!ok || read != sizeof(magic))
    {
        return false;
    }
    // stb_truetype cannot rasterize CFF ("OTTO") outlines.
    const bool ttf = magic[0] == 0x00 && magic[1] == 0x01 && magic[2] == 0x00 && magic[3] == 0x00;
    return ttf || std::memcmp(magic, "ttcf", 4) == 0 || std::memcmp(magic, "true", 4) == 0;
}

ImFont* AddFontSize(float px)
{
    ImGuiIO& io = ImGui::GetIO();

    const HMODULE module = SelfModule();
    HRSRC resource = FindResourceA(module, MAKEINTRESOURCEA(kItcBoltTtfResourceId), MAKEINTRESOURCEA(10) /*RT_RCDATA*/);
    HGLOBAL loaded = (resource != nullptr) ? LoadResource(module, resource) : nullptr;
    const DWORD size = (resource != nullptr) ? SizeofResource(module, resource) : 0;
    void* data = (loaded != nullptr) ? LockResource(loaded) : nullptr;
    ImFontConfig primary;
    primary.FontDataOwnedByAtlas = false; // resource memory lives as long as the module
    ImFont* added = (data == nullptr || size == 0)
        ? nullptr
        : io.Fonts->AddFontFromMemoryTTF(data, static_cast<int>(size), px, &primary);
    if (added == nullptr)
    {
        mod::Log("ReplayOverlay: ITC Bolt resource unavailable, using ImGui default font");
        io.Fonts->AddFontDefault();
    }

    char windowsDir[MAX_PATH] = {};
    const UINT n = GetWindowsDirectoryA(windowsDir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        return added;
    }
    const char* faces[] = {"YuGothM.ttc", "meiryo.ttc", "msgothic.ttc"};
    for (const char* face : faces)
    {
        char path[MAX_PATH] = {};
        std::snprintf(path, sizeof(path), "%s\\Fonts\\%s", windowsDir, face);
        if (!IsStbLoadableFontFile(path))
        {
            continue;
        }
        ImFontConfig merge;
        merge.MergeMode = true;
        merge.OversampleH = 1;
        merge.OversampleV = 1;
        if (io.Fonts->AddFontFromFileTTF(path, px, &merge, io.Fonts->GetGlyphRangesJapanese()) != nullptr)
        {
            mod::Log("ReplayOverlay: Japanese glyphs merged from '%s'", path);
            return added;
        }
    }
    mod::Log("ReplayOverlay: no Japanese system font found; Japanese text will not render on the GPU path");
    return added;
}

void LoadFonts()
{
    ImGui::GetIO().Fonts->Clear();
    (void)AddFontSize(kFontPx);
    g_smallFont = AddFontSize(kSmallFontPx);
}

void ShutdownBackend()
{
    if (g_context == nullptr)
    {
        return;
    }
    EfzScopedImGuiContext context(g_context);
    if (g_device != nullptr)
    {
        ImGui_ImplDX9_Shutdown();
    }
    context.ForgetDestroyed(g_context);
    ImGui::DestroyContext(g_context);
    g_context = nullptr;
    g_contextThread = 0;
    g_device = nullptr;
    g_deviceObjectsLive = false;
    g_resetGate.UnbindBackend();
}

// Caller holds the gate. Context selection is the caller's job.
bool EnsureInited(IDirect3DDevice9* device)
{
    if (g_context != nullptr && g_device == device)
    {
        return true;
    }
    if (g_context != nullptr)
    {
        EfzScopedImGuiContext context(g_context);
        ImGui_ImplDX9_Shutdown();
        g_resetGate.UnbindBackend();
        g_device = nullptr;
        g_deviceObjectsLive = false;
        if (!ImGui_ImplDX9_Init(device))
        {
            mod::Log("ReplayOverlay: DX9 backend rebind failed device=%p", device);
            return false;
        }
        g_device = device;
        g_resetGate.BindBackend(device, GetCurrentThreadId());
        mod::Log("ReplayOverlay: DX9 backend rebound device=%p", device);
        return true;
    }

    IMGUI_CHECKVERSION();
    ImGuiContext* previous = ImGui::GetCurrentContext();
    g_context = ImGui::CreateContext();
    g_contextThread = GetCurrentThreadId();
    ImGui::SetCurrentContext(g_context);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    LoadFonts();
    const bool ok = ImGui_ImplDX9_Init(device);
    ImGui::SetCurrentContext(previous);
    if (!ok)
    {
        ImGui::DestroyContext(g_context);
        g_context = nullptr;
        g_contextThread = 0;
        return false;
    }
    g_device = device;
    g_resetGate.BindBackend(device, GetCurrentThreadId());
    mod::Log("ReplayOverlay: ImGui initialised device=%p", device);
    return true;
}

ImU32 ToImColor(std::uint32_t argb)
{
    return IM_COL32((argb >> 16) & 0xFFu, (argb >> 8) & 0xFFu, argb & 0xFFu, (argb >> 24) & 0xFFu);
}

void DrawPixelText(ImDrawList* list, const TextItem& item)
{
    const float dot = item.pixelScale;
    const std::size_t length = std::strlen(item.text);
    const float width = length == 0 ? 0.0f : (length * 6.0f - 1.0f) * dot;
    const float left = std::floor((item.x0 + item.x1 - width) * 0.5f);
    const ImU32 ink = ToImColor(item.argb);
    const ImU32 shadow = IM_COL32(0, 0, 0, (item.argb >> 24) & 0xFFu);
    for (int pass = 0; pass < 2; ++pass)
    {
        const float offset = (pass == 0) ? dot : 0.0f;
        const ImU32 color = (pass == 0) ? shadow : ink;
        for (std::size_t i = 0; i < length; ++i)
        {
            const std::uint8_t* glyph = replay::render::Glyph5x7(item.text[i]);
            if (glyph == nullptr)
            {
                continue;
            }
            const float gx = left + i * 6.0f * dot + offset;
            for (int row = 0; row < 7; ++row)
            {
                for (int col = 0; col < 5; ++col)
                {
                    if ((glyph[row] & (1u << (4 - col))) != 0)
                    {
                        const ImVec2 a(gx + col * dot, item.y + row * dot + offset);
                        list->AddRectFilled(a, ImVec2(a.x + dot, a.y + dot), color);
                    }
                }
            }
        }
    }
}

void DrawItems()
{
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    const DWORD now = GetTickCount();
    std::map<std::string, DWORD> scrollSeen;
    EnterCriticalSection(&TextLock());
    for (const TextItem& item : g_active)
    {
        if (item.pixelScale > 0.0f)
        {
            DrawPixelText(list, item);
            continue;
        }
        if (item.fillArgb != 0)
        {
            list->AddRectFilled(ImVec2(item.x0 * 2.0f, item.y * 2.0f), ImVec2(item.x1 * 2.0f, (item.y + item.h) * 2.0f),
                ToImColor(item.fillArgb));
        }
        const bool useSmall = item.smallText && g_smallFont != nullptr;
        ImFont* const itemFont = useSmall ? g_smallFont : font;
        const float itemPx = useSmall ? kSmallFontPx : kFontPx;
        const ImVec2 size = itemFont->CalcTextSizeA(itemPx, FLT_MAX, 0.0f, item.text);
        const float x0 = item.x0 * 2.0f;
        const float x1 = item.x1 * 2.0f;
        float x = x0;
        if (item.align == TextAlign::Center)
        {
            x = x0 + ((x1 - x0) - size.x) * 0.5f;
        }
        else if (item.align == TextAlign::Right)
        {
            x = x1 - size.x;
        }
        if (x < x0)
        {
            x = x0;
        }
        const float y = item.y * 2.0f + (item.h * 2.0f - size.y) * 0.5f;
        const float overflow = size.x - (x1 - x0);
        if (overflow > 0.0f && !item.noScroll)
        {
            const auto known = g_scrollStart.find(item.text);
            const DWORD start = (known != g_scrollStart.end()) ? known->second : now;
            scrollSeen[item.text] = start;
            x -= replay::motion::TextScrollOffset(overflow, now - start);
        }
        const bool clip = item.clipX1 > item.clipX0 || overflow > 0.0f;
        if (clip)
        {
            const float clipLeft = (item.clipX1 > item.clipX0) ? item.clipX0 * 2.0f : x0;
            const float clipRight = (item.clipX1 > item.clipX0) ? item.clipX1 * 2.0f : x1;
            list->PushClipRect(ImVec2(clipLeft, 0.0f), ImVec2(clipRight, 480.0f), true);
        }
        list->AddText(itemFont, itemPx, ImVec2(static_cast<float>(static_cast<int>(x)), static_cast<float>(static_cast<int>(y))),
            ToImColor(item.argb), item.text);
        if (clip)
        {
            list->PopClipRect();
        }
    }
    LeaveCriticalSection(&TextLock());
    g_scrollStart.swap(scrollSeen);
}

void Render(IDirect3DDevice9* device)
{
    EfzOverlayResetGate::Access access(g_resetGate);
    if (!access.Acquired() || !g_resetGate.CanRender(device, GetCurrentThreadId())
        || (g_context != nullptr && g_contextThread != GetCurrentThreadId()))
    {
        return;
    }

    if (!g_published.load(std::memory_order_acquire))
    {
        // Hold no DEFAULT-pool objects outside the replay menu.
        if (g_deviceObjectsLive && g_device == device)
        {
            EfzScopedImGuiContext context(g_context);
            ImGui_ImplDX9_InvalidateDeviceObjects();
            g_deviceObjectsLive = false;
        }
        return;
    }
    if (g_backendFailed.load(std::memory_order_relaxed))
    {
        return;
    }
    if (device->TestCooperativeLevel() != D3D_OK)
    {
        // Lost/not-reset device: drop DEFAULT-pool objects so whoever resets (Revival,
        // without DisplayManager's contract) can succeed; they are recreated lazily.
        if (g_deviceObjectsLive && g_device == device)
        {
            EfzScopedImGuiContext context(g_context);
            ImGui_ImplDX9_InvalidateDeviceObjects();
            g_deviceObjectsLive = false;
        }
        return;
    }

    // Only the game's 640x480 RT; skips display-mod passes into other targets.
    IDirect3DSurface9* target = nullptr;
    if (FAILED(device->GetRenderTarget(0, &target)) || target == nullptr)
    {
        return;
    }
    D3DSURFACE_DESC desc = {};
    const HRESULT descResult = target->GetDesc(&desc);
    target->Release();
    if (FAILED(descResult) || desc.Width != 640 || desc.Height != 480)
    {
        return;
    }

    if (!EnsureInited(device))
    {
        g_backendFailed.store(true);
        mod::Log("ReplayOverlay: backend init failed; software text stays active");
        return;
    }

    EfzScopedImGuiContext context(g_context);
    if (!g_deviceObjectsLive)
    {
        if (!ImGui_ImplDX9_CreateDeviceObjects())
        {
            ImGui_ImplDX9_InvalidateDeviceObjects();
            g_backendFailed.store(true);
            mod::Log("ReplayOverlay: device object creation failed; software text stays active");
            return;
        }
        g_deviceObjectsLive = true;
    }

    ImGui_ImplDX9_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(640.0f, 480.0f);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    DrawItems();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

// Pass-through: a plain D3D9 device cannot Reset while DEFAULT-pool objects exist, and
// not every resetter calls our EFZ_QueryOverlayResetApiV1 contract (Revival's own
// fullscreen/resize path, or an older DisplayManager). Release ours, then always call
// through; never refuse, delay or touch another owner's objects. Recreated lazily by the
// next frame that has text to draw.
HRESULT WINAPI HookedReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters)
{
    {
        EfzOverlayResetGate::Access access(g_resetGate);
        if (access.Acquired() && g_deviceObjectsLive && g_device == device && g_context != nullptr)
        {
            EfzScopedImGuiContext context(g_context);
            ImGui_ImplDX9_InvalidateDeviceObjects();
            g_deviceObjectsLive = false;
            mod::Log("ReplayOverlay: released device objects ahead of Reset");
        }
    }
    return g_originalReset(device, parameters);
}

HRESULT WINAPI HookedEndScene(IDirect3DDevice9* device)
{
    g_lastEndSceneTick.store(GetTickCount(), std::memory_order_relaxed);
    if (device != nullptr)
    {
        Render(device);
    }
    return g_originalEndScene(device);
}

HRESULT ResetGateResult(EfzOverlayResetGate::Result result)
{
    switch (result)
    {
    case EfzOverlayResetGate::Result::Ok: return S_OK;
    case EfzOverlayResetGate::Result::InvalidArgument: return E_INVALIDARG;
    case EfzOverlayResetGate::Result::WrongThread: return RPC_E_WRONG_THREAD;
    default: return HRESULT_FROM_WIN32(ERROR_BUSY);
    }
}

HRESULT __cdecl BeforeOverlayReset(void* device, std::uint32_t epoch)
{
    EfzOverlayResetGate::Access access(g_resetGate);
    if (!access.Acquired())
    {
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    }
    bool invalidate = false;
    const auto result = g_resetGate.BeforeReset(device, epoch, GetCurrentThreadId(), invalidate);
    if (result != EfzOverlayResetGate::Result::Ok)
    {
        return ResetGateResult(result);
    }
    if (invalidate)
    {
        EfzScopedImGuiContext context(g_context);
        ImGui_ImplDX9_InvalidateDeviceObjects();
        g_deviceObjectsLive = false;
        g_resetGate.DidInvalidateBackend();
    }
    return S_OK;
}

HRESULT __cdecl AfterOverlayReset(void* device, std::uint32_t epoch, std::uint32_t ready)
{
    EfzOverlayResetGate::Access access(g_resetGate);
    if (!access.Acquired())
    {
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    }
    if (ready > 1)
    {
        return E_INVALIDARG;
    }
    const auto result = g_resetGate.AfterReset(device, epoch, GetCurrentThreadId(), 0);
    if (result != EfzOverlayResetGate::Result::Ok || !ready)
    {
        return ResetGateResult(result);
    }
    return ResetGateResult(g_resetGate.AfterReset(device, epoch, GetCurrentThreadId(), 1));
}

bool FindEndSceneTarget()
{
    HMODULE d3d9Module = GetModuleHandleA("d3d9.dll");
    if (d3d9Module == nullptr)
    {
        d3d9Module = LoadLibraryA("d3d9.dll");
    }
    auto direct3dCreate9 = (d3d9Module != nullptr)
        ? reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(d3d9Module, "Direct3DCreate9"))
        : nullptr;
    IDirect3D9* d3d = (direct3dCreate9 != nullptr) ? direct3dCreate9(D3D_SDK_VERSION) : nullptr;
    if (d3d == nullptr)
    {
        mod::Log("ReplayOverlay: Direct3DCreate9 unavailable");
        return false;
    }

    WNDCLASSA windowClass = {};
    windowClass.lpfnWndProc = DefWindowProcA;
    windowClass.hInstance = GetModuleHandleA(nullptr);
    windowClass.lpszClassName = "EFZ_REPLAY_D3D9_DUMMY";
    (void)RegisterClassA(&windowClass);
    const HWND window = CreateWindowExA(0, windowClass.lpszClassName, "efz_replay_dummy", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 64, 64, nullptr, nullptr, windowClass.hInstance, nullptr);
    if (window == nullptr)
    {
        d3d->Release();
        mod::Log("ReplayOverlay: dummy window creation failed");
        return false;
    }

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = 2;
    pp.BackBufferHeight = 2;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    pp.hDeviceWindow = window;
    IDirect3DDevice9* device = nullptr;
    // FPU_PRESERVE: otherwise D3D9 drops this thread's x87 precision; EFZ's math is x87 doubles.
    const HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &device);
    if (SUCCEEDED(hr) && device != nullptr)
    {
        g_endSceneTarget = (*reinterpret_cast<void***>(device))[42];
        g_resetTarget = (*reinterpret_cast<void***>(device))[16];
        device->Release();
    }
    else
    {
        mod::Log("ReplayOverlay: dummy CreateDevice failed hr=0x%08X", static_cast<unsigned>(hr));
    }
    d3d->Release();
    DestroyWindow(window);
    UnregisterClassA(windowClass.lpszClassName, windowClass.hInstance);
    return g_endSceneTarget != nullptr;
}
}

bool EnsureHookInstalled()
{
    if (g_hookInstalled || g_hookAttempted)
    {
        return g_hookInstalled;
    }
    g_hookAttempted = true;

    if (!FindEndSceneTarget()
        || !replay::hook::Install(g_endSceneTarget, reinterpret_cast<void*>(&HookedEndScene),
            reinterpret_cast<void**>(&g_originalEndScene), "EndScene"))
    {
        g_originalEndScene = nullptr;
        return false;
    }
    if (g_resetTarget == nullptr
        || !replay::hook::Install(g_resetTarget, reinterpret_cast<void*>(&HookedReset),
            reinterpret_cast<void**>(&g_originalReset), "Reset"))
    {
        g_originalReset = nullptr;
        g_resetTarget = nullptr;
    }
    // The first EndScene runs inside the next present: count the overlay as live now so that
    // frame already goes to the RT instead of flashing the software font.
    g_lastEndSceneTick.store(GetTickCount(), std::memory_order_relaxed);
    g_hookInstalled = true;
    mod::Log("ReplayOverlay: EndScene hook installed target=%p original=%p reset=%p",
        g_endSceneTarget, reinterpret_cast<void*>(g_originalEndScene), g_resetTarget);
    return true;
}

bool IsAvailable()
{
    return g_hookInstalled
        && !g_backendFailed.load(std::memory_order_relaxed)
        && GetTickCount() - g_lastEndSceneTick.load(std::memory_order_relaxed) < kEndSceneStaleMs;
}

void BeginFrame()
{
    EnterCriticalSection(&TextLock());
    g_staging.clear();
    LeaveCriticalSection(&TextLock());
}

void Submit(const TextItem& item)
{
    if (item.text[0] == '\0' && item.fillArgb == 0)
    {
        return;
    }
    EnterCriticalSection(&TextLock());
    if (g_staging.size() < kMaxItems)
    {
        g_staging.push_back(item);
    }
    LeaveCriticalSection(&TextLock());
}

void CommitFrame()
{
    EnterCriticalSection(&TextLock());
    g_active.swap(g_staging);
    g_staging.clear();
    g_published.store(!g_active.empty(), std::memory_order_release);
    LeaveCriticalSection(&TextLock());
}

void Clear()
{
    if (!g_published.load(std::memory_order_acquire))
    {
        return;
    }
    EnterCriticalSection(&TextLock());
    g_staging.clear();
    g_active.clear();
    g_published.store(false, std::memory_order_release);
    LeaveCriticalSection(&TextLock());
}

void Shutdown()
{
    Clear();
    if (g_hookInstalled)
    {
        (void)MH_DisableHook(g_endSceneTarget);
        (void)MH_RemoveHook(g_endSceneTarget);
        if (g_resetTarget != nullptr)
        {
            (void)MH_DisableHook(g_resetTarget);
            (void)MH_RemoveHook(g_resetTarget);
        }
        g_hookInstalled = false;
    }
    EfzOverlayResetGate::Access access(g_resetGate);
    if (access.Acquired() && (g_contextThread == 0 || g_contextThread == GetCurrentThreadId()))
    {
        ShutdownBackend();
    }
}
}

extern "C" const EfzOverlayResetApiV1* __cdecl EFZ_QueryOverlayResetApiV1(void)
{
    static const EfzOverlayResetApiV1 api = {
        sizeof(EfzOverlayResetApiV1), 1,
        &replay::render::d3d9::BeforeOverlayReset,
        &replay::render::d3d9::AfterOverlayReset,
    };
    return &api;
}
