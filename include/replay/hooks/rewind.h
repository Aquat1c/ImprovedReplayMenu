#pragma once

#include <cstdint>

namespace replay::hooks
{
// Game thread. Installs the Revival detours once; false if off or this build is unknown.
bool EnsureRewindInstalled();
void RemoveRewind();
bool IsRewindAvailable();
bool IsHiddenTick();
const char* GetRewindTag();
// EFZ's DirectInput keyboard, indexed by DIK code. Zeroed + false without focus.
bool ReadRewindKeyboard(std::uint8_t (&keys)[256]);
void NoteBattleFault();
}
