#include "replay/core/replay_file_metadata.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace replay::file
{
bool HasRepExtension(const std::string& path)
{
    return HasRepExtension(std::filesystem::path(path));
}

bool HasRepExtension(const std::filesystem::path& path)
{
    const std::filesystem::path p(path);
    std::string ext = p.extension().string();
    for (char& c : ext)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return ext == ".rep";
}

bool IsReplayFileUsable(const std::string& path, std::uintmax_t* outSizeBytes)
{
    return IsReplayFileUsable(std::filesystem::path(path), outSizeBytes);
}

bool IsReplayFileUsable(const std::filesystem::path& path, std::uintmax_t* outSizeBytes)
{
    if (outSizeBytes != nullptr)
    {
        *outSizeBytes = 0;
    }

    if (!HasRepExtension(path))
    {
        return false;
    }

    std::error_code ec;
    const std::filesystem::path p(path);
    if (!std::filesystem::exists(p, ec) || ec)
    {
        return false;
    }
    if (!std::filesystem::is_regular_file(p, ec) || ec)
    {
        return false;
    }

    const std::uintmax_t sizeBytes = std::filesystem::file_size(p, ec);
    if (ec)
    {
        return false;
    }
    if (sizeBytes == 0)
    {
        return false;
    }

    if (outSizeBytes != nullptr)
    {
        *outSizeBytes = sizeBytes;
    }
    return true;
}

bool ParseReplayHeaderMetadata(const std::string& path, ReplayHeaderMetadata* outMetadata)
{
    return ParseReplayHeaderMetadata(std::filesystem::path(path), outMetadata);
}

bool ParseReplayHeaderMetadata(const std::filesystem::path& path, ReplayHeaderMetadata* outMetadata)
{
    if (outMetadata == nullptr)
    {
        return false;
    }
    *outMetadata = {};

    if (!HasRepExtension(path))
    {
        return false;
    }

    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    LARGE_INTEGER sizeBytes = {};
    unsigned char header[12] = {};
    DWORD got = 0;
    const bool read = GetFileSizeEx(file, &sizeBytes) != FALSE && ReadFile(file, header, sizeof(header), &got, nullptr) != FALSE;
    CloseHandle(file);

    // Vanilla read path consumes at least:
    // p1 char, p2 char, stage token, optional stage, p1 color, p2 color (u16 each).
    // The game tests the token's low byte as a signed char. A file with only the header has no match in it.
    const bool stageFollows = got > 4 && static_cast<std::int8_t>(header[4]) > 100;
    const DWORD headerBytes = stageFollows ? 12u : 10u;
    if (!read || got < headerBytes || sizeBytes.QuadPart < static_cast<LONGLONG>(headerBytes) + 2)
    {
        return false;
    }
    const auto u16 = [&header](std::size_t at) {
        return static_cast<std::uint16_t>(header[at] | (header[at + 1] << 8));
    };

    ReplayHeaderMetadata parsed = {};
    parsed.fileSizeBytes = static_cast<std::uintmax_t>(sizeBytes.QuadPart);
    parsed.p1CharacterId = u16(0);
    parsed.p2CharacterId = u16(2);
    parsed.stageToken = u16(4);
    std::size_t colors = 6;
    if (stageFollows)
    {
        parsed.hasSpecialStageToken = true;
        parsed.specialStageToken = parsed.stageToken;
        parsed.stageId = u16(6);
        colors = 8;
    }
    else
    {
        parsed.stageId = parsed.stageToken;
    }
    parsed.p1Color = u16(colors);
    parsed.p2Color = u16(colors + 2);

    parsed.valid = true;
    *outMetadata = parsed;
    return true;
}

bool IsLikelyValidCharacterId(std::uint16_t id)
{
    // Vanilla character selection uses rand() % 24; 24 is the boss, which a replay can also hold.
    return id <= 24;
}

ReplayLength MeasureReplayLength(const std::uint8_t* bytes, std::size_t size)
{
    const auto u16 = [bytes](std::size_t at) {
        return static_cast<std::uint32_t>(bytes[at] | (bytes[at + 1] << 8));
    };
    const auto u32 = [&](std::size_t at) { return u16(at) | (u16(at + 2) << 16); };

    ReplayLength length;
    if (bytes == nullptr || size < 16)
    {
        return length;
    }
    length.format = ReplayFormat::NativeTape;
    std::size_t nativeEnd = size;
    if (u32(size - 4) == 0xFFFFFFFFu)
    {
        const std::uint64_t t = u32(size - 8);
        if (t + 8 <= size - 8)
        {
            const std::uint64_t n = u32(static_cast<std::size_t>(t) + 4);
            if (t + 8 + 2 * n + 8 == size)
            {
                if (n != 0)
                {
                    length.frames = static_cast<std::uint32_t>(n / 2);
                    length.exact = true;
                    length.format = ReplayFormat::RevivalPairs;
                    return length;
                }
                // No saved inputs, so no count: the native stream ends where the footer starts.
                length.format = ReplayFormat::EmptyExtension;
                const auto footerAt = [&](std::size_t bytesLong, const std::uint16_t* pattern, std::size_t words) {
                    if (t < 12 + bytesLong)
                    {
                        return false;
                    }
                    const std::size_t start = static_cast<std::size_t>(t) - bytesLong;
                    for (std::size_t i = 0; i < bytesLong / 2; ++i)
                    {
                        if (u16(start + 2 * i) != pattern[i % words])
                        {
                            return false;
                        }
                    }
                    return true;
                };
                static constexpr std::uint16_t kFooter480[] = {0, 9999, 0, 0, 0, 0, 0, 0};
                static constexpr std::uint16_t kFooter240[] = {0, 0, 0, 9999};
                nativeEnd = static_cast<std::size_t>(t);
                if (footerAt(480, kFooter480, 8))
                {
                    nativeEnd -= 480;
                }
                else if (footerAt(240, kFooter240, 4))
                {
                    nativeEnd -= 240;
                }
            }
        }
    }
    // Header: p1, p2, token, [stage if (char)token > 100], p1pal, p2pal.
    const std::size_t headerWords = (static_cast<std::int8_t>(bytes[4]) > 100) ? 6 : 5;
    for (std::size_t at = headerWords * 2; at + 1 < nativeEnd; at += 2)
    {
        const std::uint32_t word = u16(at);
        if (word == 9999 || word == 3332)
        {
            ++length.frames;
        }
    }
    return length;
}

ReplayLength MeasureReplayLength(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return MeasureReplayLength(bytes.data(), bytes.size());
}
}
