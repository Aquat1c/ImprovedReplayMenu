#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace replay::file
{
struct ReplayHeaderMetadata
{
    bool valid = false;
    std::uintmax_t fileSizeBytes = 0;

    std::uint16_t p1CharacterId = 0;
    std::uint16_t p2CharacterId = 0;
    std::uint16_t stageToken = 0;
    bool hasSpecialStageToken = false;
    std::uint16_t specialStageToken = 0;
    std::uint16_t stageId = 0;
    std::uint16_t p1Color = 0;
    std::uint16_t p2Color = 0;
};

bool HasRepExtension(const std::string& path);
bool HasRepExtension(const std::filesystem::path& path);
bool IsReplayFileUsable(const std::string& path, std::uintmax_t* outSizeBytes);
bool IsReplayFileUsable(const std::filesystem::path& path, std::uintmax_t* outSizeBytes);
bool ParseReplayHeaderMetadata(const std::string& path, ReplayHeaderMetadata* outMetadata);
bool ParseReplayHeaderMetadata(const std::filesystem::path& path, ReplayHeaderMetadata* outMetadata);
bool IsLikelyValidCharacterId(std::uint16_t id);

// What a .rep holds. All of them start with the same header.
//   NativeTape:     vanilla or very old Revival, the native stream and nothing else.
//   EmptyExtension: old Revival, the native stream, then a footer and an extension saving no inputs.
//   RevivalPairs:   Revival, also saving both players' inputs. The footer is 480 bytes, 240 on older builds.
enum class ReplayFormat : std::uint8_t { Unknown, NativeTape, EmptyExtension, RevivalPairs };

// Playback length in battle ticks. Revival-recorded files end with a trailer
//   pad, u32 rngSeed@T, u32 N, u16 inputs[N] (P1,P2 per tick), u32 T, u32 0xFFFFFFFF
// giving the exact count N/2. Other files carry no count: every non-paused frame
// stores an HP pair whose second word is 9999 (or 3332), so those are counted as
// an estimate (RNG draws and inputs are interleaved without markers).
struct ReplayLength
{
    std::uint32_t frames = 0;
    bool exact = false;
    ReplayFormat format = ReplayFormat::Unknown;
};
ReplayLength MeasureReplayLength(const std::uint8_t* bytes, std::size_t size);
ReplayLength MeasureReplayLength(const std::filesystem::path& path);
}
