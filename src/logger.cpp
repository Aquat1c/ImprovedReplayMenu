#include "logger.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#if !defined(EFZ_REPLAY_LOG_CONSOLE)
#define EFZ_REPLAY_LOG_CONSOLE 0
#endif

#if !defined(EFZ_REPLAY_LOG_FILE)
#define EFZ_REPLAY_LOG_FILE 0
#endif

namespace
{
constexpr bool kEnableConsoleLogging = (EFZ_REPLAY_LOG_CONSOLE != 0);
constexpr bool kEnableFileLogging = (EFZ_REPLAY_LOG_FILE != 0);

std::mutex g_logMutex;
bool g_consoleReady = false;
bool g_consoleOwned = false;
FILE* g_logFile = nullptr;
std::string g_logFilePath;
std::unordered_set<std::string> g_loggedOnceKeys;
std::unordered_map<std::string, std::uint64_t> g_lastLogByKey;
std::unordered_map<std::string, std::uint64_t> g_lastLogByMessage;
constexpr DWORD kDefaultDuplicateThrottleMs = 1200;
constexpr std::size_t kMaxThrottleMapEntries = 4096;

std::string BuildLogPathFromModule(HMODULE moduleHandle)
{
    char modulePath[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(moduleHandle, modulePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        return "efz_replay_mod.log";
    }

    std::string path(modulePath);
    const std::size_t slashPos = path.find_last_of("\\/");
    if (slashPos == std::string::npos)
    {
        return "efz_replay_mod.log";
    }

    path.resize(slashPos + 1);
    path += "efz_replay_mod.log";
    return path;
}

void WriteLineUnlocked(const char* line)
{
    OutputDebugStringA(line);

    if (g_consoleReady)
    {
        fputs(line, stdout);
        fflush(stdout);
    }

    if (g_logFile != nullptr)
    {
        fputs(line, g_logFile);
        fflush(g_logFile);
    }
}

bool PrepareConsoleStreams()
{
    FILE* outStream = nullptr;
    FILE* errStream = nullptr;
    FILE* inStream = nullptr;
    if (freopen_s(&outStream, "CONOUT$", "w", stdout) != 0 || outStream == nullptr)
    {
        return false;
    }
    if (freopen_s(&errStream, "CONOUT$", "w", stderr) != 0 || errStream == nullptr)
    {
        return false;
    }
    if (freopen_s(&inStream, "CONIN$", "r", stdin) != 0 || inStream == nullptr)
    {
        return false;
    }

    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    return true;
}

bool EnsureConsole()
{
    if (!kEnableConsoleLogging)
    {
        return false;
    }

    if (g_consoleReady)
    {
        return true;
    }

    // Do not hijack an existing console (e.g. from another mod).
    if (GetConsoleWindow() != nullptr)
    {
        return false;
    }

    if (AllocConsole() == FALSE)
    {
        return false;
    }

    if (!PrepareConsoleStreams())
    {
        FreeConsole();
        return false;
    }

    g_consoleOwned = true;
    return true;
}

std::string FormatLogMessage(const char* fmt, va_list args)
{
    if (fmt == nullptr)
    {
        return std::string();
    }

    char message[2048] = {};
    va_list copy;
    va_copy(copy, args);
    const int written = vsnprintf(message, sizeof(message), fmt, copy);
    va_end(copy);
    if (written <= 0)
    {
        return std::string();
    }
    return std::string(message);
}

void WriteFormattedLogUnlocked(const std::string& message)
{
    if (message.empty())
    {
        return;
    }

    char line[2200] = {};
    snprintf(line, sizeof(line), "[efz_replay_mod] %s\n", message.c_str());
    WriteLineUnlocked(line);
}

bool ShouldEmitWithIntervalUnlocked(
    std::unordered_map<std::string, std::uint64_t>* map,
    const std::string& key,
    DWORD intervalMs)
{
    if (map == nullptr || key.empty())
    {
        return true;
    }

    const std::uint64_t now = static_cast<std::uint64_t>(GetTickCount64());
    auto it = map->find(key);
    if (it != map->end())
    {
        const std::uint64_t elapsed = now - it->second;
        if (elapsed < static_cast<std::uint64_t>(intervalMs))
        {
            return false;
        }
        it->second = now;
        return true;
    }

    if (map->size() > kMaxThrottleMapEntries)
    {
        map->clear();
    }
    (*map)[key] = now;
    return true;
}
}

namespace mod
{
bool InitializeLogger(HMODULE moduleHandle)
{
    std::lock_guard<std::mutex> lock(g_logMutex);

    if (!g_consoleReady && kEnableConsoleLogging)
    {
        if (EnsureConsole())
        {
            SetConsoleTitleA("EFZ Replay Mod Logger");
            g_consoleReady = true;
        }
    }

    if (g_logFilePath.empty())
    {
        g_logFilePath = BuildLogPathFromModule(moduleHandle);
    }

    if (g_logFile == nullptr && kEnableFileLogging)
    {
        FILE* file = nullptr;
        if (fopen_s(&file, g_logFilePath.c_str(), "a") == 0 && file != nullptr)
        {
            g_logFile = file;
        }
    }

    if (g_consoleReady)
    {
        WriteLineUnlocked("[efz_replay_mod] console logging enabled\n");
    }
    else if (kEnableConsoleLogging)
    {
        WriteLineUnlocked("[efz_replay_mod] console logging unavailable (existing console not used)\n");
    }
    else
    {
        WriteLineUnlocked("[efz_replay_mod] console logging disabled for this build\n");
    }

    if (g_logFile != nullptr)
    {
        WriteLineUnlocked("[efz_replay_mod] file logging enabled\n");
    }
    else if (kEnableFileLogging)
    {
        WriteLineUnlocked("[efz_replay_mod] file logging unavailable\n");
    }
    else
    {
        WriteLineUnlocked("[efz_replay_mod] file logging disabled for this build\n");
    }
    WriteLineUnlocked("[efz_replay_mod] logger initialized\n");
    return true;
}

void ShutdownLogger()
{
    std::lock_guard<std::mutex> lock(g_logMutex);

    if (g_logFile != nullptr)
    {
        fputs("[efz_replay_mod] logger shutting down\n", g_logFile);
        fflush(g_logFile);
        fclose(g_logFile);
        g_logFile = nullptr;
    }

    if (g_consoleReady && g_consoleOwned)
    {
        FreeConsole();
    }
    g_consoleReady = false;
    g_consoleOwned = false;
    g_loggedOnceKeys.clear();
    g_lastLogByKey.clear();
    g_lastLogByMessage.clear();
}

void Log(const char* fmt, ...)
{
    if (!kEnableConsoleLogging && !kEnableFileLogging)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(g_logMutex);

    if (!g_consoleReady && kEnableConsoleLogging && EnsureConsole())
    {
        SetConsoleTitleA("EFZ Replay Mod Logger");
        g_consoleReady = true;
        WriteLineUnlocked("[efz_replay_mod] logger auto-initialized console output\n");
    }

    if (g_logFile == nullptr && kEnableFileLogging)
    {
        FILE* fallback = nullptr;
        const char* fallbackPath = g_logFilePath.empty() ? "efz_replay_mod.log" : g_logFilePath.c_str();
        if (fopen_s(&fallback, fallbackPath, "a") == 0 && fallback != nullptr)
        {
            g_logFile = fallback;
            WriteLineUnlocked("[efz_replay_mod] logger auto-opened fallback log file\n");
        }
    }

    va_list args;
    va_start(args, fmt);
    const std::string message = FormatLogMessage(fmt, args);
    va_end(args);
    if (message.empty())
    {
        return;
    }

    if (!ShouldEmitWithIntervalUnlocked(&g_lastLogByMessage, message, kDefaultDuplicateThrottleMs))
    {
        return;
    }

    WriteFormattedLogUnlocked(message);
}

void LogOnce(const char* key, const char* fmt, ...)
{
    if (!kEnableConsoleLogging && !kEnableFileLogging)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (key == nullptr || *key == '\0')
    {
        return;
    }

    const std::string keyString(key);
    if (g_loggedOnceKeys.find(keyString) != g_loggedOnceKeys.end())
    {
        return;
    }

    va_list args;
    va_start(args, fmt);
    const std::string message = FormatLogMessage(fmt, args);
    va_end(args);
    if (message.empty())
    {
        return;
    }

    if (g_loggedOnceKeys.size() > kMaxThrottleMapEntries)
    {
        g_loggedOnceKeys.clear();
    }
    g_loggedOnceKeys.insert(keyString);
    WriteFormattedLogUnlocked(message);
}

void LogThrottled(const char* key, DWORD intervalMs, const char* fmt, ...)
{
    if (!kEnableConsoleLogging && !kEnableFileLogging)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (key == nullptr || *key == '\0')
    {
        return;
    }
    if (intervalMs == 0)
    {
        intervalMs = kDefaultDuplicateThrottleMs;
    }

    if (!ShouldEmitWithIntervalUnlocked(&g_lastLogByKey, key, intervalMs))
    {
        return;
    }

    va_list args;
    va_start(args, fmt);
    const std::string message = FormatLogMessage(fmt, args);
    va_end(args);
    if (message.empty())
    {
        return;
    }

    WriteFormattedLogUnlocked(message);
}
}
