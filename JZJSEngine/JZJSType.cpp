#include "JZJSType.h"
#include "JZJSEngine.h"
#include <assert.h>

using namespace std;

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
// JSValue 形参 = **借用引用(borrowed)**, 与 quickjs C API 的 argv 约定一致:
// 绑定层把调用方的 argv[i] 原样交过来, 不 JS_DupValue。于是:
//   * 只是"读一下"(jsValueToBytes / JS_IsFunction / JS_Call ...) —— 直接用, 不要 free;
//   * 要**留下来**(挂监听、存成员、跨调用持有) —— 自己 JS_DupValue 一次(如 JZJSEventSink::on),
//     并在释放时 free 自己那一份。
// ⚠ 绝不能对形参调 JS_FreeValue —— 那减的是**调用方(JS 侧)**那一份的引用计数, 会提前析构
//   => UAF(历史 bug: JZJSHttpResponse::write/end, 表现是 res.end() 发完响应后引擎 0xC0000005)。
// 反过来若在这里 JS_DupValue(改成"移交所有权"), 则所有只读形参的调用点都会每次泄漏一个引用
// (如 socket.write(buf) 循环 -> 缓冲区永不回收), 代价更大。
template <>
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

// 对象指针形参: opaque 里存的就是 JZObjectPointer* —— **借用**, 不 dup 不 delete
// (所有权在该 JS 对象的终结器: 它负责 delete 守卫与底层对象)。
// 这里只取值; "是不是活着的对象指针"由 checkFunctionArg 在调用前校验(见 JSType_objectPointer)。
template <>
JZObjectPointer* fromJSValue(JSContext* ctx, JSValueConst value)
{
    if (JS_IsNull(value) || JS_IsUndefined(value))
        return nullptr;

    JSClassID class_id = 0;
    void* opaque = JS_GetAnyOpaque(value, &class_id);
    if (!opaque)
        return nullptr;

    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    if (!engine || !engine->isGuardedClass((int)class_id))
    {
        JZ_ASSERT_X(0, "fromJSValue<JZObjectPointer*>: 不是对象指针(上层应已校验)");
        return nullptr;
    }
    return (JZObjectPointer*)opaque;
}

JSValue createJSValue(JSContext* ctx, const std::string& ctypeid)
{
    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    int type = engine->ctypeidToType(ctypeid);
    return engine->createValue(type, 0, nullptr);
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