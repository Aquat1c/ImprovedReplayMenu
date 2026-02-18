#pragma once

#include <windows.h>

namespace crash
{
bool InitializeCrashHandler(HMODULE moduleHandle);
void ShutdownCrashHandler();

int HandleSehException(const char* scope, EXCEPTION_POINTERS* exceptionPointers);
}
