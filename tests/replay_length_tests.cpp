#include "replay/core/replay_file_metadata.h"

#include <cstdio>
#include <cstdint>
#include <vector>

namespace
{
int g_failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); ++g_failures; } } while (0)

void Put16(std::vector<std::uint8_t>& b, std::uint32_t v)
{
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}

void Put32(std::vector<std::uint8_t>& b, std::uint32_t v)
{
    Put16(b, v & 0xFFFFu);
    Put16(b, v >> 16);
}

// Header (5 words) + `frames` frames of HP pair + two inputs + one RNG draw.
std::vector<std::uint8_t> Payload(int frames)
{
    std::vector<std::uint8_t> b;
    for (std::uint32_t w : {3u, 7u, 12u, 1u, 2u})
    {
        Put16(b, w);
    }
    for (int i = 0; i < frames; ++i)
    {
        Put16(b, 1000);
        Put16(b, 9999);
        Put16(b, 0x10);
        Put16(b, 0x00);
        Put16(b, 1234);
    }
    return b;
}

void AppendTrailer(std::vector<std::uint8_t>& b, std::uint32_t ticks)
{
    for (int i = 0; i < 30; ++i)
    {
        Put16(b, 0);
        Put16(b, 9999);
        for (int k = 0; k < 6; ++k)
        {
            Put16(b, 0);
        }
    }
    const std::uint32_t t = static_cast<std::uint32_t>(b.size());
    Put32(b, 0x12345678u);
    Put32(b, ticks * 2);
    for (std::uint32_t i = 0; i < ticks * 2; ++i)
    {
        Put16(b, 0);
    }
    Put32(b, t);
    Put32(b, 0xFFFFFFFFu);
}
}

int main()
{
    using replay::file::MeasureReplayLength;

    std::vector<std::uint8_t> trailer = Payload(120);
    AppendTrailer(trailer, 121);
    auto length = MeasureReplayLength(trailer.data(), trailer.size());
    CHECK(length.exact);
    CHECK(length.frames == 121);

    std::vector<std::uint8_t> plain = Payload(500);
    length = MeasureReplayLength(plain.data(), plain.size());
    CHECK(!length.exact);
    CHECK(length.frames == 500);

    std::vector<std::uint8_t> broken = Payload(40);
    Put32(broken, 3);
    Put32(broken, 0xFFFFFFFFu);
    length = MeasureReplayLength(broken.data(), broken.size());
    CHECK(!length.exact);
    CHECK(length.frames == 40);

    CHECK(MeasureReplayLength(nullptr, 0).frames == 0);

    using replay::file::ReplayFormat;
    CHECK(MeasureReplayLength(plain.data(), plain.size()).format == ReplayFormat::NativeTape);
    CHECK(MeasureReplayLength(trailer.data(), trailer.size()).format == ReplayFormat::RevivalPairs);

    // Old Revival: a footer and an extension saving nothing. Not "exactly 0 frames", and the
    // footer's own 9999s must not count. 480-byte footer first, then the 240-byte one.
    std::vector<std::uint8_t> empty480 = Payload(77);
    AppendTrailer(empty480, 0);
    length = MeasureReplayLength(empty480.data(), empty480.size());
    CHECK(!length.exact);
    CHECK(length.frames == 77);
    CHECK(length.format == ReplayFormat::EmptyExtension);

    std::vector<std::uint8_t> empty240 = Payload(77);
    for (int i = 0; i < 120; ++i)
    {
        Put16(empty240, (i % 4 == 3) ? 9999u : 0u);
    }
    const std::uint32_t offset240 = static_cast<std::uint32_t>(empty240.size());
    Put32(empty240, 0);
    Put32(empty240, 0);
    Put32(empty240, offset240);
    Put32(empty240, 0xFFFFFFFFu);
    length = MeasureReplayLength(empty240.data(), empty240.size());
    CHECK(!length.exact);
    CHECK(length.frames == 77);
    CHECK(length.format == ReplayFormat::EmptyExtension);

    std::vector<std::uint8_t> pairs240 = Payload(10);
    for (int i = 0; i < 120; ++i)
    {
        Put16(pairs240, (i % 4 == 3) ? 9999u : 0u);
    }
    const std::uint32_t pairsOffset = static_cast<std::uint32_t>(pairs240.size());
    Put32(pairs240, 0x1234u);
    Put32(pairs240, 22);
    for (int i = 0; i < 22; ++i)
    {
        Put16(pairs240, 0);
    }
    Put32(pairs240, pairsOffset);
    Put32(pairs240, 0xFFFFFFFFu);
    length = MeasureReplayLength(pairs240.data(), pairs240.size());
    CHECK(length.exact);
    CHECK(length.frames == 11);
    CHECK(length.format == ReplayFormat::RevivalPairs);

    if (g_failures == 0)
    {
        std::puts("PASS: trailer length exact, plain files counted by HP markers");
    }
    return g_failures == 0 ? 0 : 1;
}
