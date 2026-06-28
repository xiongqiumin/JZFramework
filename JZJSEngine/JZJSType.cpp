#include "JZJSType.h"

#include <assert.h>

#include "JZJSEngine.h"

// CFunctionParam
CFunctionParam::CFunctionParam()
{
    type = JSType_none;
}

// CFunction
CFunction::CFunction()
{
}

CFunction::~CFunction()
{
}

// from js
JSValue fromJSValue(JSContext* ctx, JSValueConst value)
{
    return value;
}

template <>
bool fromJSValue(JSContext* ctx, JSValueConst value)
{
    int pres = JS_ToBool(ctx, value);
    return pres;
}

template <>
int fromJSValue(JSContext* ctx, JSValueConst value)
{
    int pres = 0;
    JS_ToInt32(ctx, &pres, value);
    return pres;
}

template <>
double fromJSValue(JSContext* ctx, JSValueConst value)
{
    double pres = 0;
    JS_ToFloat64(ctx, &pres, value);
    return pres;
}

template <>
std::string fromJSValue(JSContext* ctx, JSValueConst value)
{
    std::string ret;
    const char* ptr = JS_ToCString(ctx, value);
    ret = ptr;
    JS_FreeCString(ctx, ptr);
    return ret;
}

template <>
Json::Value fromJSValue(JSContext* ctx, JSValueConst value)
{
    JSValue js_str = JS_JSONStringify(ctx, value, JS_UNDEFINED, JS_UNDEFINED);

    std::string str;
    bool ret = js_to_value(ctx, js_str, str);
    JZ_ASSERT(ret);

    Json::Value ret_value;

    Json::Reader reader;
    ret = reader.parse(str, ret_value);
    JZ_ASSERT(ret);

    return ret_value;
}

// to js
JSValue createJSValue(JSContext* ctx, const std::string& ctypeid)
{
    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    int id = engine->ctypeidToType(ctypeid);
    return engine->createValue(id);
}

template <>
JSValue toJSValue(JSContext* ctx, const JSValue& value)
{
    return value;
}

template <>
JSValue toJSValue(JSContext* ctx, const bool& value)
{
    return JS_NewBool(ctx, value);
}

template <>
JSValue toJSValue(JSContext* ctx, const int& value)
{
    return JS_NewInt32(ctx, value);
}

template <>
JSValue toJSValue(JSContext* ctx, const double& value)
{
    return JS_NewFloat64(ctx, value);
}

template <>
JSValue toJSValue(JSContext* ctx, const std::string& value)
{
    return JS_NewString(ctx, value.c_str());
}

template <>
JSValue toJSValue(JSContext* ctx, const Json::Value& value)
{
    Json::FastWriter writer;
    string str = writer.write(value);

    JSValue ret = JS_ParseJSON(ctx, str.data(), str.length(), "json");
    assert(!JS_IsException(ret));

    return ret;
}

bool js_check_ctypeid(JSContext* ctx, JSValueConst js_value, const std::string& ctypeid)
{
    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    int type = engine->ctypeidToType(ctypeid);
    if (!engine->checkType(js_value, type))
        return false;

    return true;
}

bool js_has_prop(JSContext* ctx, JSValueConst js_value, const std::string& prop)
{
    JSValue js_prop = JS_GetPropertyStr(ctx, js_value, prop.c_str());
    bool ret = !JS_IsUndefined(js_prop);
    JS_FreeValue(ctx, js_prop);
    return ret;
}