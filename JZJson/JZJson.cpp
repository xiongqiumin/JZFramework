#include "JZJson.h"
#include "JZDebug.h"

JZDebug operator<<(JZDebug d, const Json::Value& value)
{
    Json::FastWriter writer;
    d << writer.write(value);
    return d;
}