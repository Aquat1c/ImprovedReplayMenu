#include <windows.h>

#include <cstdint>

#include "crash_handler.h"
#include "logger.h"
#include "replay/hooks/menu_hooks.h"

namespace
{
bool IsGameMainLoopRunning()
{
    constexpr std::uintptr_t kVaMainLoopFlag = 0x79014C; // efz.exe sets it right before its message loop
#if defined(_MSC_VER)
    __try
    {
        return *reinterpret_cast<const volatile int*>(kVaMainLoopFlag) != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
#else
    return *reinterpret_cast<const volatile int*>(kVaMainLoopFlag) != 0;
#endif
}

// Every mod's startup thread begins at the same moment. InGameNetplay patches Revival by stopping all
// other threads and gives up for good if one of them cannot be stopped, so stay asleep through that:
// no hooks, no console, and no thread exiting while it runs. The replay screen is not reachable yet.
void WaitOutOtherModsStartup()
{
    for (int waited = 0; waited < 10000 && !IsGameMainLoopRunning(); waited += 50)
    {
        Sleep(50);
    }
    Sleep(1000);
}

DWORD WINAPI InitializeModThread(LPVOID moduleHandleRaw)
{
    WaitOutOtherModsStartup();
#if defined(_MSC_VER)
    __try
    {
#endif
    const auto moduleHandle = static_cast<HMODULE>(moduleHandleRaw);
    mod::InitializeLogger(moduleHandle);
    mod::Log("DllMain::InitializeModThread: module attached at %p", moduleHandle);
    (void)crash::InitializeCrashHandler(moduleHandle);

    if (!replay::InstallHooks())
    {
        mod::Log("DllMain::InitializeModThread: InstallHooks failed");
    }
    else
    {
        mod::Log("DllMain::InitializeModThread: InstallHooks succeeded");
    }

    return 0;
#if defined(_MSC_VER)
    }
    __except (crash::HandleSehException("DllMain::InitializeModThread", GetExceptionInformation()))
    {
        mod::Log("DllMain::InitializeModThread: recovered from structured exception");
        return 1;
    }
#endif
}
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ulReasonForCall, LPVOID lpReserved)
{
    if (ulReasonForCall == DLL_PROCESS_ATTACH)
    {
        OutputDebugStringA("[efz_replay_mod] DllMain::DLL_PROCESS_ATTACH received\n");
    }

    switch (ulReasonForCall)
    {
    case DLL_PROCESS_ATTACH:
    {
        DisableThreadLibraryCalls(hModule);
        HANDLE thread = CreateThread(nullptr, 0, InitializeModThread, hModule, 0, nullptr);
        if (thread != nullptr)
        {
            OutputDebugStringA("[efz_replay_mod] DllMain::DLL_PROCESS_ATTACH: init thread created\n");
            CloseHandle(thread);
        }
        else
        {
            OutputDebugStringA("[efz_replay_mod] DllMain::DLL_PROCESS_ATTACH: failed to create init thread\n");
        }
        break;
    }
    case DLL_PROCESS_DETACH:
        mod::Log(
            "DllMain::DLL_PROCESS_DETACH: module=%p lpReserved=%p (processTerminating=%d)",
            hModule,
            lpReserved,
            (lpReserved != nullptr) ? 1 : 0);
        replay::CleanupRuntimeArtifacts();
        if (lpReserved == nullptr)
        {
            replay::RemoveHooks();
        }
        crash::ShutdownCrashHandler();
        mod::ShutdownLogger();
        break;
    default:
        break;
    }

    return TRUE;
}

extern "C" __declspec(dllexport) void EFZReplayMenuShowStubMessageBox(HWND owner)
{
    replay::ShowInProgressMessage(owner);
}
