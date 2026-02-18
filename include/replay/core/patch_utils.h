#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace replay::patch
{
struct PatchRecord
{
    std::uintptr_t address = 0;
    std::vector<std::uint8_t> originalBytes;
};

bool WriteProcessMemoryLocal(std::uintptr_t address, const std::uint8_t* data, std::size_t size);
bool ApplyPatch(
    std::uintptr_t address,
    const std::vector<std::uint8_t>& expectedBytes,
    const std::vector<std::uint8_t>& patchedBytes,
    const char* label,
    std::vector<PatchRecord>* patchRecords);
void RestorePatches(std::vector<PatchRecord>* patchRecords);

}
