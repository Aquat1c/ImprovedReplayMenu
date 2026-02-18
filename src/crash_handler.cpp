#include "crash_handler.h"

#include "logger.h"

#include <DbgHelp.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
PVOID g_vectoredHandler = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_previousUnhandledFilter = nullptr;
HMODULE g_moduleHandle = nullptr;

const char* ExceptionCodeName(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
        return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT:
        return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DENORMAL_OPERAND:
        return "EXCEPTION_FLT_DENORMAL_OPERAND";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INEXACT_RESULT:
        return "EXCEPTION_FLT_INEXACT_RESULT";
    case EXCEPTION_FLT_INVALID_OPERATION:
        return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW:
        return "EXCEPTION_FLT_OVERFLOW";
    case EXCEPTION_FLT_STACK_CHECK:
        return "EXCEPTION_FLT_STACK_CHECK";
    case EXCEPTION_FLT_UNDERFLOW:
        return "EXCEPTION_FLT_UNDERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:
        return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:
        return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_INVALID_DISPOSITION:
        return "EXCEPTION_INVALID_DISPOSITION";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_SINGLE_STEP:
        return "EXCEPTION_SINGLE_STEP";
    case EXCEPTION_STACK_OVERFLOW:
        return "EXCEPTION_STACK_OVERFLOW";
    default:
        return "UNKNOWN_EXCEPTION";
    }
}

std::string BuildDumpPath(HMODULE moduleHandle)
{
    constexpr const char* kDumpFileName = "efz_replay_mod_crash.dmp";

    char modulePath[MAX_PATH] = {};
    if (GetModuleFileNameA(moduleHandle, modulePath, MAX_PATH) == 0)
    {
        return kDumpFileName;
    }

    std::string path(modulePath);
    const std::size_t slashPos = path.find_last_of("\\/");
    if (slashPos != std::string::npos)
    {
        path.resize(slashPos + 1);
    }
    else
    {
        path.clear();
    }

    path += kDumpFileName;
    return path;
}

void LogExceptionPointers(const char* scope, EXCEPTION_POINTERS* ep)
{
    if (ep == nullptr || ep->ExceptionRecord == nullptr)
    {
        mod::Log("CrashHandler::%s: null EXCEPTION_POINTERS", (scope != nullptr) ? scope : "(null)");
        return;
    }

    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    mod::Log(
        "CrashHandler::%s: code=0x%08X name=%s flags=0x%08X address=0x%p params=%u",
        (scope != nullptr) ? scope : "(null)",
        static_cast<unsigned>(er->ExceptionCode),
        ExceptionCodeName(er->ExceptionCode),
        static_cast<unsigned>(er->ExceptionFlags),
        er->ExceptionAddress,
        static_cast<unsigned>(er->NumberParameters));

    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
    {
        mod::Log(
            "CrashHandler::%s: access-violation type=%llu faultAddr=0x%p",
            (scope != nullptr) ? scope : "(null)",
            static_cast<unsigned long long>(er->ExceptionInformation[0]),
            reinterpret_cast<void*>(er->ExceptionInformation[1]));
    }

#if defined(_M_IX86)
    if (ep->ContextRecord != nullptr)
    {
        const CONTEXT* ctx = ep->ContextRecord;
        mod::Log(
            "CrashHandler::%s: EIP=0x%08X ESP=0x%08X EBP=0x%08X EAX=0x%08X EBX=0x%08X ECX=0x%08X EDX=0x%08X ESI=0x%08X EDI=0x%08X",
            (scope != nullptr) ? scope : "(null)",
            static_cast<unsigned>(ctx->Eip),
            static_cast<unsigned>(ctx->Esp),
            static_cast<unsigned>(ctx->Ebp),
            static_cast<unsigned>(ctx->Eax),
            static_cast<unsigned>(ctx->Ebx),
            static_cast<unsigned>(ctx->Ecx),
            static_cast<unsigned>(ctx->Edx),
            static_cast<unsigned>(ctx->Esi),
            static_cast<unsigned>(ctx->Edi));
    }
#endif
}

bool WriteMinidump(const char* scope, EXCEPTION_POINTERS* ep)
{
    if (ep == nullptr || ep->ExceptionRecord == nullptr)
    {
        return false;
    }

    const std::string dumpPath = BuildDumpPath((g_moduleHandle != nullptr) ? g_moduleHandle : GetModuleHandleA(nullptr));
    HANDLE dumpFile = CreateFileA(
        dumpPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (dumpFile == INVALID_HANDLE_VALUE)
    {
        mod::Log(
            "CrashHandler::%s: CreateFileA failed for dump '%s' err=%lu",
            (scope != nullptr) ? scope : "(null)",
            dumpPath.c_str(),
            static_cast<unsigned long>(GetLastError()));
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION mei = {};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;

    const MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
        MiniDumpNormal |
        MiniDumpWithThreadInfo);
    const BOOL ok = MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        dumpFile,
        dumpType,
        &mei,
        nullptr,
        nullptr);
    const DWORD err = GetLastError();
    CloseHandle(dumpFile);

    if (ok == FALSE)
    {
        mod::Log(
            "CrashHandler::%s: MiniDumpWriteDump failed path='%s' err=%lu",
            (scope != nullptr) ? scope : "(null)",
            dumpPath.c_str(),
            static_cast<unsigned long>(err));
        return false;
    }

    mod::Log(
        "CrashHandler::%s: minidump written path='%s'",
        (scope != nullptr) ? scope : "(null)",
        dumpPath.c_str());
    return true;
}

LONG CALLBACK VectoredExceptionHandlerProc(EXCEPTION_POINTERS* ep)
{
    if (ep == nullptr || ep->ExceptionRecord == nullptr)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION
        || code == EXCEPTION_ILLEGAL_INSTRUCTION
        || code == EXCEPTION_STACK_OVERFLOW
        || code == EXCEPTION_INT_DIVIDE_BY_ZERO)
    {
        LogExceptionPointers("Vectored", ep);
#if defined(EFZ_REPLAY_CRASH_DUMP_VECTORED)
        static LONG s_vectoredDumpWritten = 0;
        if (InterlockedCompareExchange(&s_vectoredDumpWritten, 1, 0) == 0)
        {
            (void)WriteMinidump("Vectored", ep);
            mod::Log("CrashHandler::Vectored: minidump captured for first serious fault");
        }
#endif
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI UnhandledExceptionFilterProc(EXCEPTION_POINTERS* ep)
{
    LogExceptionPointers("Unhandled", ep);
    (void)WriteMinidump("Unhandled", ep);
    return EXCEPTION_EXECUTE_HANDLER;
}
}

namespace crash
{
bool InitializeCrashHandler(HMODULE moduleHandle)
{
    g_moduleHandle = moduleHandle;
    mod::Log("CrashHandler::Initialize: module=%p", moduleHandle);

    g_previousUnhandledFilter = SetUnhandledExceptionFilter(UnhandledExceptionFilterProc);
    mod::Log(
        "CrashHandler::Initialize: previous unhandled filter=%p",
        reinterpret_cast<void*>(g_previousUnhandledFilter));

    g_vectoredHandler = AddVectoredExceptionHandler(1, VectoredExceptionHandlerProc);
    mod::Log("CrashHandler::Initialize: vectored handler=%p", g_vectoredHandler);
    return true;
}

void ShutdownCrashHandler()
{
    mod::Log("CrashHandler::Shutdown: begin");

    if (g_vectoredHandler != nullptr)
    {
        (void)RemoveVectoredExceptionHandler(g_vectoredHandler);
        mod::Log("CrashHandler::Shutdown: removed vectored handler=%p", g_vectoredHandler);
        g_vectoredHandler = nullptr;
    }

    SetUnhandledExceptionFilter(g_previousUnhandledFilter);
    mod::Log(
        "CrashHandler::Shutdown: restored unhandled filter=%p",
        reinterpret_cast<void*>(g_previousUnhandledFilter));
    g_previousUnhandledFilter = nullptr;
    g_moduleHandle = nullptr;
}

int HandleSehException(const char* scope, EXCEPTION_POINTERS* exceptionPointers)
{
    LogExceptionPointers(scope, exceptionPointers);
    (void)WriteMinidump(scope, exceptionPointers);
    return EXCEPTION_EXECUTE_HANDLER;
}
}
