#include "replay/core/replay_name_format.h"

#include "replay/core/text_utils.h"

#include <cctype>
#include <iterator>

namespace
{
bool IsAsciiDigits(const std::string& value)
{
    if (value.empty())
    {
        return false;
    }
    for (char c : value)
    {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0)
        {
            return false;
        }
    }
    return true;
}

bool ParsePlayerSegment(const std::string& raw, std::string* outName, std::string* outCharacter)
{
    if (outName == nullptr || outCharacter == nullptr)
    {
        return false;
    }

    const std::string segment = replay::text::TrimAscii(raw);
    if (segment.size() < 5)
    {
        return false;
    }

    const std::size_t closePos = segment.find_last_of(')');
    const std::size_t openPos = segment.find_last_of('(');
    if (closePos == std::string::npos || openPos == std::string::npos || closePos != segment.size() - 1 || openPos >= closePos)
    {
        return false;
    }

    std::string name = replay::text::TrimAscii(segment.substr(0, openPos));
    std::string character = replay::text::TrimAscii(segment.substr(openPos + 1, closePos - openPos - 1));
    if (name.empty() || character.empty())
    {
        return false;
    }

    *outName = name;
    *outCharacter = character;
    return true;
}

std::string FormatDate(const std::string& ymd)
{
    if (ymd.size() != 8)
    {
        return ymd;
    }
    return ymd.substr(0, 4) + "-" + ymd.substr(4, 2) + "-" + ymd.substr(6, 2);
}

std::string FormatTime(const std::string& hms)
{
    if (hms.size() == 4)
    {
        return hms.substr(0, 2) + ":" + hms.substr(2, 2);
    }
    if (hms.size() != 6)
    {
        return hms;
    }
    return hms.substr(0, 2) + ":" + hms.substr(2, 2) + ":" + hms.substr(4, 2);
}

constexpr const char* kCharacterNames[] = {"Rumi", "Ayu", "Mai", "Makoto", "Akane", "Mayu", "Nagamori", "Misaki",
    "Shiori", "Sayuri", "NayukiS", "Mio", "Doppel", "Kaori", "Ikumi", "Mishio", "Akiko", "NayukiA", "Unknown", "Kanna",
    "Kano", "Minagi", nullptr, "Misuzu"};
constexpr int kCharactersByName[] = {
    4, 16, 1, 12, 14, 19, 20, 13, 2, 3, 5, 21, 11, 7, 15, 23, 6, 17, 10, 0, 9, 8, 18};
}

namespace replay::name
{
std::string ExtractReplayStem(const std::string& pathOrFilename)
{
    std::string stem = pathOrFilename;
    const std::size_t slash = stem.find_last_of("\\/");
    if (slash != std::string::npos)
    {
        stem = stem.substr(slash + 1);
    }

    const std::size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos)
    {
        stem = stem.substr(0, dot);
    }
    return replay::text::TrimAscii(stem);
}

bool TryParseConcreteReplayName(const std::string& stem, ParsedConcreteReplayName* outParsed)
{
    if (outParsed == nullptr)
    {
        return false;
    }

    *outParsed = {};
    const std::string input = replay::text::TrimAscii(stem);
    if (input.size() < 21)
    {
        return false;
    }

    // YYYYMMDD-HHMMSS, or YYYYMMDD-HHMM on older netplay builds.
    const std::size_t timeLength = (input[13] == ' ') ? 4 : (input[15] == ' ') ? 6 : 0;
    if (input[8] != '-' || timeLength == 0)
    {
        return false;
    }

    const std::string ymd = input.substr(0, 8);
    const std::string hms = input.substr(9, timeLength);
    if (!IsAsciiDigits(ymd) || !IsAsciiDigits(hms))
    {
        return false;
    }

    const std::string rest = replay::text::TrimAscii(input.substr(10 + timeLength));
    const std::size_t vsPos = rest.rfind(" vs ");
    if (vsPos == std::string::npos)
    {
        return false;
    }

    const std::string p1Segment = rest.substr(0, vsPos);
    const std::string p2Segment = rest.substr(vsPos + 4);

    std::string p1Name;
    std::string p1Char;
    std::string p2Name;
    std::string p2Char;
    if (!ParsePlayerSegment(p1Segment, &p1Name, &p1Char) || !ParsePlayerSegment(p2Segment, &p2Name, &p2Char))
    {
        return false;
    }

    outParsed->matched = true;
    outParsed->dateYmd = ymd;
    outParsed->timeHms = hms;
    outParsed->p1Name = p1Name;
    outParsed->p1Character = p1Char;
    outParsed->p2Name = p2Name;
    outParsed->p2Character = p2Char;
    return true;
}

std::string FormatReplayDisplayName(const std::string& stem, bool prettifyConcrete, bool* outConcreteMatch)
{
    ParsedConcreteReplayName parsed = {};
    const bool matched = TryParseConcreteReplayName(stem, &parsed);
    if (outConcreteMatch != nullptr)
    {
        *outConcreteMatch = matched;
    }

    if (!matched)
    {
        return replay::text::TrimAscii(stem);
    }

    if (!prettifyConcrete)
    {
        return replay::text::TrimAscii(stem);
    }

    const std::string out = FormatDate(parsed.dateYmd) + " " + FormatTime(parsed.timeHms) + "  "
         + parsed.p1Name + " (" + parsed.p1Character + ") vs "
         + parsed.p2Name + " (" + parsed.p2Character + ")";
    return out;
}

bool TryParsePlainReplayName(const std::string& stem, std::string* outP1, std::string* outP2)
{
    const std::string input = replay::text::TrimAscii(stem);
    const std::size_t vsPos = input.find(" vs ");
    if (vsPos == std::string::npos || vsPos == 0 || input.find(" vs ", vsPos + 4) != std::string::npos
        || (input.size() >= 4 && IsAsciiDigits(input.substr(0, 4))))
    {
        return false;
    }
    *outP1 = replay::text::TrimAscii(input.substr(0, vsPos));
    *outP2 = replay::text::TrimAscii(input.substr(vsPos + 4));
    const std::size_t open = outP2->rfind(" (");
    if (open != std::string::npos && outP2->back() == ')'
        && IsAsciiDigits(outP2->substr(open + 2, outP2->size() - open - 3)))
    {
        outP2->resize(open);
    }
    return !outP1->empty() && !outP2->empty();
}

int ReplayCharacterIdFromName(const std::string& name)
{
    for (int id = 0; id < static_cast<int>(std::size(kCharacterNames)); ++id)
    {
        if (kCharacterNames[id] != nullptr && name == kCharacterNames[id])
        {
            return id;
        }
    }
    return -1;
}

const char* ReplayCharacterName(int id)
{
    return (id >= 0 && id < static_cast<int>(std::size(kCharacterNames))) ? kCharacterNames[id] : nullptr;
}

int StepReplayCharacterFilter(int current, int step)
{
    const int count = static_cast<int>(std::size(kCharactersByName));
    int position = count;
    for (int i = 0; i < count; ++i)
    {
        if (kCharactersByName[i] == current)
        {
            position = i;
        }
    }
    position = (position + (step > 0 ? 1 : count)) % (count + 1);
    return position == count ? -1 : kCharactersByName[position];
}
}
