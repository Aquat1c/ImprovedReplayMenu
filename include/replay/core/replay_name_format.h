#pragma once

#include <string>

namespace replay::name
{
struct ParsedConcreteReplayName
{
    bool matched = false;
    std::string dateYmd;
    std::string timeHms;
    std::string p1Name;
    std::string p1Character;
    std::string p2Name;
    std::string p2Character;
};

std::string ExtractReplayStem(const std::string& pathOrFilename);
bool TryParseConcreteReplayName(const std::string& stem, ParsedConcreteReplayName* outParsed);
std::string FormatReplayDisplayName(const std::string& stem, bool prettifyConcrete, bool* outConcreteMatch);
// "P1 vs P2", with an optional " (n)" counter on P2. Names starting with a date are not plain names.
bool TryParsePlainReplayName(const std::string& stem, std::string* outP1, std::string* outP2);

// Character ids as replay headers store them, which is not the id order of the character objects.
// Name <-> id, measured on 7,299 named replays with no exceptions. Unknown names give -1, unknown ids nullptr.
int ReplayCharacterIdFromName(const std::string& name);
const char* ReplayCharacterName(int id);
int StepReplayCharacterFilter(int current, int step);
}
