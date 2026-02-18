#include "logger.h"

#include <cstdarg>
#include <cstdio>

namespace mod
{
void Log(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    std::fputc('\n', stderr);
    va_end(args);
}
}
