#pragma once

#include <string>

namespace replay::text
{
std::string TrimAscii(std::string value);
bool ParseIntToken(const std::string& token, int* outValue);
std::string ToUpperAscii(std::string value);
std::string TruncateWithEllipsis(const std::string& value, std::size_t maxLen);
}

