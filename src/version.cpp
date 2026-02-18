#include "mod_version.h"

namespace replay::version
{
const char* BuildTime()
{
    return __DATE__ " " __TIME__;
}
}
