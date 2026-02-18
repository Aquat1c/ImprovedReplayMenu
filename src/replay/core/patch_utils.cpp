#include "replay/core/patch_utils.h"

#include "logger.h"

#include <cstring>
#include <windows.h>

namespace
{
// No C++ objects here, so SEH is allowed: an access violation means another module
// re-protected the page between our VirtualProtect and this copy.
bool CopyGuarded(std::uintptr_t address, const std::uint8_t* data, std::size_t size)
{
    __try
    {
        std::memcpy(reinterpret_cast<void*>(address), data, size);
        return true;
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        return false;
    }
}
}

namespace replay::patch
{
bool WriteProcessMemoryLocal(std::uintptr_t address, const std::uint8_t* data, std::size_t size)
{
    if (address == 0 || data == nullptr || size == 0)
    {
        mod::Log(
            "PatchUtils::WriteProcessMemoryLocal: invalid args addr=0x%08X data=%d size=%u",
            static_cast<unsigned>(address),
            (data != nullptr) ? 1 : 0,
            static_cast<unsigned>(size));
        return false;
    }

    // Other mods patch the same efz.exe pages (InGameNetplay's title vtable shares our
    // replay/battle vtable page) with their own VirtualProtect save/restore from their
    // own init threads. So: never restore the old protection (that could fault their
    // write), and if their restore lands between our protect and our write, take the
    // access violation, unprotect again and retry.
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        DWORD oldProtect = 0;
        if (VirtualProtect(reinterpret_cast<void*>(address), size, PAGE_EXECUTE_READWRITE, &oldProtect) == FALSE)
        {
            mod::Log("PatchUtils::WriteProcessMemoryLocal: VirtualProtect failed addr=0x%08X", static_cast<unsigned>(address));
            return false;
        }
        if (CopyGuarded(address, data, size))
        {
            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), size);
            return true;
        }
        mod::Log("PatchUtils::WriteProcessMemoryLocal: page re-protected under us addr=0x%08X, retrying",
            static_cast<unsigned>(address));
    }
    return false;
}

bool ApplyPatch(
    std::uintptr_t address,
    const std::vector<std::uint8_t>& expectedBytes,
    const std::vector<std::uint8_t>& patchedBytes,
    const char* label,
    std::vector<PatchRecord>* patchRecords)
{
    if (patchRecords == nullptr)
    {
        mod::Log("PatchUtils::ApplyPatch: patchRecords null label='%s'", (label != nullptr) ? label : "(null)");
        return false;
    }
    if (patchedBytes.empty())
    {
        mod::Log("PatchUtils::ApplyPatch: skip empty patch label='%s'", (label != nullptr) ? label : "(null)");
        return true;
    }

    std::vector<std::uint8_t> currentBytes(patchedBytes.size());
    std::memcpy(currentBytes.data(), reinterpret_cast<void*>(address), currentBytes.size());

    if (!expectedBytes.empty() && expectedBytes != currentBytes)
    {
        mod::Log("PatchUtils::ApplyPatch: expected mismatch label='%s' addr=0x%08X", (label != nullptr) ? label : "(null)", static_cast<unsigned>(address));
        return false;
    }

    if (!WriteProcessMemoryLocal(address, patchedBytes.data(), patchedBytes.size()))
    {
        mod::Log("PatchUtils::ApplyPatch: write failed label='%s' addr=0x%08X", (label != nullptr) ? label : "(null)", static_cast<unsigned>(address));
        return false;
    }

    patchRecords->push_back(PatchRecord{address, std::move(currentBytes)});
    mod::Log("PatchUtils::ApplyPatch: applied label='%s' addr=0x%08X size=%u", (label != nullptr) ? label : "(null)", static_cast<unsigned>(address), static_cast<unsigned>(patchedBytes.size()));
    return true;
}

void RestorePatches(std::vector<PatchRecord>* patchRecords)
{
    if (patchRecords == nullptr)
    {
        return;
    }

    for (auto it = patchRecords->rbegin(); it != patchRecords->rend(); ++it)
    {
        (void)WriteProcessMemoryLocal(it->address, it->originalBytes.data(), it->originalBytes.size());
    }
    mod::Log("PatchUtils::RestorePatches: restored count=%u", static_cast<unsigned>(patchRecords->size()));
    patchRecords->clear();
}
}
