#pragma once

#include <windows.h>

namespace mod
{
bool InitializeLogger(HMODULE moduleHandle);
void ShutdownLogger();
void Log(const char* fmt, ...);
void LogOnce(const char* key, const char* fmt, ...);
void LogThrottled(const char* key, DWORD intervalMs, const char* fmt, ...);

#if !defined(EFZ_REPLAY_LOG_VERBOSE)
#define EFZ_REPLAY_LOG_VERBOSE 0
#endif
template <typename... Args>
void LogVerbose(const char* fmt, Args... args)
{
    if constexpr (EFZ_REPLAY_LOG_VERBOSE != 0)
    {
        Log(fmt, args...);
    }
}
}

