#pragma once

#include <windows.h>

namespace replay
{
bool InstallHooks();
void RemoveHooks();
bool AreHooksInstalled();
void CleanupRuntimeArtifacts();

void ShowInProgressMessage(HWND owner);
}
