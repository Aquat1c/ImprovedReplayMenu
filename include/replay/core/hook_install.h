#pragma once

#include "logger.h"

#include "MinHook.h"

#include <windows.h>

namespace replay::hook
{
// Create + enable a MinHook detour that stays correct when another mod hooks the
// same entry concurrently: if the entry changed between create and enable
// (MH_ERROR_TARGET_CHANGED), rebuild the trampoline from the new entry and retry.
inline bool Install(void* target, void* detour, void** original, const char* label)
{
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
    {
        mod::Log("Hook::Install: MH_Initialize failed %s label=%s", MH_StatusToString(init), label);
        return false;
    }
    for (int attempt = 0; attempt < 10; ++attempt)
    {
        MH_STATUS status = MH_CreateHook(target, detour, original);
        if (status == MH_OK)
        {
            status = MH_EnableHook(target);
            if (status == MH_OK)
            {
                mod::Log("Hook::Install: %s target=%p attempts=%d", label, target, attempt + 1);
                return true;
            }
            (void)MH_RemoveHook(target);
        }
        if (status != MH_ERROR_TARGET_CHANGED)
        {
            mod::Log("Hook::Install: %s failed %s target=%p", label, MH_StatusToString(status), target);
            return false;
        }
        Sleep(10); // another installer is mid-write
    }
    mod::Log("Hook::Install: %s target=%p kept changing; giving up", label, target);
    return false;
}
}
