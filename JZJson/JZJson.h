#ifndef JZ_JSON_H_
#define JZ_JSON_H_

#include "json/json.h"
#include "JZJsonStream.h"

class JZDebug;
JZDebug operator<<(JZDebug d, const Json::Value& value);

#endif