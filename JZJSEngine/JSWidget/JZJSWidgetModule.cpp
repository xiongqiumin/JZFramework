#include "JZJSWidgetModule.h"

#include "JZJSBind.h"
#include "JZJSWidget.h"
#include "JZApplication.h"
#include "JZDesktopServices.h"
#include "JZDebug.h"   // LOG_W(透明控件类找不到全局类时的告警)
#include "JZDir.h"
#include "JZWidget.h"
#include "JZWindow.h"
#include "JZMainWindow.h"
#include "JZMenuBar.h"
#include "JZMenu.h"
#include "JZAction.h"
#include "JZDialog.h"
#include "JZMessageBox.h"
#include "JZFileDialog.h"
#include "JZSettings.h"
#include "JZLayout.h"
#include "JZPainter.h"
#include "JZPen.h"
#include "JZBrush.h"
#include "JZFont.h"
#include "JZFontMetrics.h"   // JZFont.measureText/textWidth/... (JZFontMetrics 门面)
#include "JZColor.h"
#include "JZMatrix2D.h"

#include "JZPoint.h"
#include "JZSize.h"
#include "JZRect.h"
#include "JZImage.h"

#include "JZEvent.h"
#include "JZMouseEvent.h"
#include "JZKeyEvent.h"
#include "JZResizeEvent.h"
#include "JZShowEvent.h"
#include "JZHideEvent.h"
#include "JZImeEvent.h"
#include "JZPainterEvent.h"
#include "JZCloseEvent.h"

#include "JZLabel.h"
#include "JZPushButton.h"
#include "JZLineEdit.h"
#include "JZDateEdit.h"
#include "JZCheckBox.h"
#include "JZRadioButton.h"
#include "JZSlider.h"
#include "JZProgressBar.h"
#include "JZComboBox.h"
#include "JZSpinBox.h"
#include "JZDoubleSpinBox.h"
#include "JZGroupBox.h"
#include "JZChart.h"
#include "JZSwitch.h"
#include "JZTextEdit.h"
#include "JZImageLabel.h"
#include "JZListWidget.h"
#include "JZTableWidget.h"
#include "JZStackedWidget.h"
#include "JZTabWidget.h"
#include "JZSplitter.h"
#include "JZScrollArea.h"
#include "JZScrollBar.h"
#include "JZJSUiLoader.h"

#include <string>
#include <cstring>

// 颜色名/十六进制解析 (供 JZPainter 的 setPen/setBrush、JZPen/JZBrush 构造器与测试复用;
// 声明在 JZJSWidget.h —— JZJSWidget.cpp 的构造器要用同一份实现, 别在别处再抄一遍)
JZColor jzParseColor(const std::string& s)
{
    if (!s.empty() && s[0] == '#')
    {
        std::string h = s.substr(1);
        if (h.size() == 3)
        {
            int r = std::stoi(h.substr(0, 1) + h.substr(0, 1), nullptr, 16);
            int g = std::stoi(h.substr(1, 1) + h.substr(1, 1), nullptr, 16);
            int b = std::stoi(h.substr(2, 1) + h.substr(2, 1), nullptr, 16);
            return JZColor((uint8_t)r, (uint8_t)g, (uint8_t)b);
        }
        if (h.size() >= 6)
        {
            int r = std::stoi(h.substr(0, 2), nullptr, 16);
            int g = std::stoi(h.substr(2, 2), nullptr, 16);
            int b = std::stoi(h.substr(4, 2), nullptr, 16);
            uint8_t a = 255;
            if (h.size() >= 8) a = (uint8_t)std::stoi(h.substr(6, 2), nullptr, 16);
            return JZColor((uint8_t)r, (uint8_t)g, (uint8_t)b, a);
        }
    }
    if (s == "black")   return JZColor(JZColor::Black);
    if (s == "white")   return JZColor(JZColor::White);
    if (s == "red")     return JZColor(JZColor::Red);
    if (s == "green")   return JZColor(JZColor::Green);
    if (s == "blue")    return JZColor(JZColor::Blue);
    if (s == "yellow")  return JZColor(JZColor::Yellow);
    if (s == "gray" || s == "grey") return JZColor(JZColor::Gray);
    if (s == "cyan")    return JZColor(JZColor::Cyan);
    if (s == "magenta") return JZColor(JZColor::Magenta);
    if (s == "transparent") return JZColor(JZColor::Transparent);
    return JZColor(0, 0, 0);
}

// ---------------------------------------------------------------------------
// JS 数组形参 -> std::vector(供 JZChart 这类"数据用数组传入"的接口)
//
// 为什么这些接口用 JSValue 形参而不是直接写 vector 形参:
//   形参类型校验走 JZJSEngine::checkFunctionArg, 它按 jsValueType + canConvert 判断;
//   而 JS 数组的 class id(JS_CLASS_ARRAY)不在引擎类型表里 -> jsValueType 得到 JSType_none,
//   会被判成"类型不匹配"。故这些形参用 JSValue(JSType_oriValue, 不做类型检查),
//   再用模块持有的 engine 取 ctx 自行转换 —— 顺带能给出明确的错误信息。
// 约定: 出错抛异常(由 JZJSEngine::call 的 try/catch 转成 JS 异常)。
// ---------------------------------------------------------------------------

static std::vector<double> jzJsToDoubleArray(JSContext* ctx, JSValueConst value, const char* what)
{
    if (!JS_IsArray(value))
        throw std::runtime_error(std::string(what) + ": 需要数字数组");

    return fromJSValue<std::vector<double>>(ctx, value);
}

static std::vector<std::vector<double>> jzJsToDoubleMatrix(JSContext* ctx, JSValueConst value, const char* what)
{
    if (!JS_IsArray(value))
        throw std::runtime_error(std::string(what) + ": 需要二维数组 [[1,2],[3,4]]");

    std::vector<std::vector<double>> list;
    int64_t rows = 0;
    JS_GetLength(ctx, value, &rows);
    for (int64_t i = 0; i < rows; i++)
    {
        JSValue row = JS_GetPropertyInt64(ctx, value, i);
        if (!JS_IsArray(row))
        {
            JS_FreeValue(ctx, row);
            throw std::runtime_error(std::string(what) + ": 每个序列应是数字数组");
        }
        list.push_back(fromJSValue<std::vector<double>>(ctx, row));
        JS_FreeValue(ctx, row);
    }
    return list;
}

static std::vector<std::string> jzJsToStringArray(JSContext* ctx, JSValueConst value, const char* what)
{
    if (!JS_IsArray(value))
        throw std::runtime_error(std::string(what) + ": 需要字符串数组");

    std::vector<std::string> list;
    int64_t n = 0;
    JS_GetLength(ctx, value, &n);
    for (int64_t i = 0; i < n; i++)
    {
        JSValue item = JS_GetPropertyInt64(ctx, value, i);
        if (!JS_IsString(item))
        {
            JS_FreeValue(ctx, item);
            throw std::runtime_error(std::string(what) + ": 元素必须是字符串");
        }
        const char* s = JS_ToCString(ctx, item);
        list.push_back(s ? s : "");
        if (s)
            JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, item);
    }
    return list;
}

// ---------- 列表/表格的"整行/整项"数据转换 ----------
// 这层的接口只跟"整行/整项"的普通 JS 值打交道: 这里负责 JS 数组 <->
// std::vector<std::string> 的互转。单元格允许 字符串/数字/布尔(统一成文本),
// 其余类型一律报错 —— 免得对象被悄悄变成 "[object Object]" 存进去。

static std::vector<std::string> jzJsToRow(JSContext* ctx, JSValueConst value, const char* what)
{
    if (!JS_IsArray(value))
        throw std::runtime_error(std::string(what) + ": 需要数组(一行)");

    std::vector<std::string> row;
    int64_t n = 0;
    JS_GetLength(ctx, value, &n);
    for (int64_t i = 0; i < n; i++)
    {
        JSValue item = JS_GetPropertyInt64(ctx, value, i);
        if (JS_IsString(item) || JS_IsNumber(item) || JS_IsBool(item))
        {
            const char* s = JS_ToCString(ctx, item);
            row.push_back(s ? s : "");
            if (s)
                JS_FreeCString(ctx, s);
        }
        else
        {
            JS_FreeValue(ctx, item);
            throw std::runtime_error(std::string(what) + ": 单元格需要字符串/数字/布尔");
        }
        JS_FreeValue(ctx, item);
    }
    return row;
}

static std::vector<std::vector<std::string>> jzJsToRows(JSContext* ctx, JSValueConst value, const char* what)
{
    if (!JS_IsArray(value))
        throw std::runtime_error(std::string(what) + ": 需要二维数组(整表)");

    std::vector<std::vector<std::string>> rows;
    int64_t n = 0;
    JS_GetLength(ctx, value, &n);
    for (int64_t i = 0; i < n; i++)
    {
        JSValue row = JS_GetPropertyInt64(ctx, value, i);
        rows.push_back(jzJsToRow(ctx, row, what));
        JS_FreeValue(ctx, row);
    }
    return rows;
}

// 文本快照 -> JS 数组的数组(每行一个新数组; 视图侧再拷一份, 两边不共享可变结构)
static JSValue jzRowsToJs(JSContext* ctx, const std::vector<std::vector<std::string>>& rows)
{
    JSValue array = JS_NewArray(ctx);
    for (size_t r = 0; r < rows.size(); r++)
    {
        JSValue row = JS_NewArray(ctx);
        for (size_t c = 0; c < rows[r].size(); c++)
            JS_SetPropertyUint32(ctx, row, (uint32_t)c, JS_NewString(ctx, rows[r][c].c_str()));
        JS_SetPropertyUint32(ctx, array, (uint32_t)r, row);
    }
    return array;
}

// ---------- 透明控件类: op 槽(见 JZJSWidgetModule.h 的说明) ----------

// 基构造器 `__JZWidgetObject`: TS 侧的 class 用 super() 调它 —— 造出的实例:
//   * class_id = 我们的 op class(quickjs 的 JS_SetOpaque 只对自定义 class 生效, 普通对象没有 opaque);
//   * 原型 = new_target.prototype(即 JS 侧那个具体的类, 于是方法/instanceof/访问器全按 JS 语义走);
//   * op 槽先留空, 由 init_c_xxx(this) 填。
static JSValue jzWidgetOpCtor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    JZJSWidgetModule* module = engine ? engine->widgetModule() : nullptr;
    if (!module)
        return JS_ThrowInternalError(ctx, "JZWidget 基构造器: 引擎没有装配 widget 模块");

    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (!JS_IsObject(proto))
    {
        JS_FreeValue(ctx, proto);
        return JS_ThrowTypeError(ctx, "JZWidget 基构造器: 只能作为 JS 类的基类被 super() 调用");
    }

    JSValue obj = JS_NewObjectProtoClass(ctx, proto, module->widgetOpClassId());
    JS_FreeValue(ctx, proto);
    JS_SetOpaque(obj, nullptr);
    return obj;
}

// 终结: 释放 op 槽。所有权一直在 JS 侧 —— 底层对象还活着(守卫非空)就由这里 delete;
// 已经先被 C++ 删过(守卫被 ~JZObject 置空)就只放守卫本身, 天然防重复删除。
// 这里**不需要**引擎/模块(用 JS_GetAnyOpaque 而不是 JS_GetOpaque): 引擎关闭时模块可能已经卸掉,
// 但对象的收尾不能漏。
static void jzWidgetOpFinalizer(JSRuntime* rt, JSValue val)
{
    JSClassID class_id = 0;
    void* opaque = JS_GetAnyOpaque(val, &class_id);
    JZObjectPointer* guard = (JZObjectPointer*)opaque;
    if (!guard)
        return;

    if (guard->object())
        delete guard->object();
    delete guard;
}

void JZJSWidgetModule::initWidgetOpClass()
{
    JSContext* ctx = m_engine->context();
    JSRuntime* rt = JS_GetRuntime(ctx);

    m_opClassId = JS_NewClassID(rt, &m_opClassId);

    JSClassDef def = {};
    def.class_name = "JZWidgetObject";
    def.finalizer = jzWidgetOpFinalizer;
    JS_NewClass(rt, m_opClassId, &def);

    // 让引擎认得这个类: 它没进 m_typeMap, 但它的 opaque 同样是守卫(见 JZJSEngine::isGuardedClass)
    m_engine->setWidgetOpClassId((int)m_opClassId);

    // 基构造器 + 它自己的原型(TS 侧 class 的 "extends" 目标)
    JSValue base = JS_NewCFunction2(ctx, jzWidgetOpCtor, "__JZWidgetObject", 0, JS_CFUNC_constructor, 0);
    JSValue baseProto = JS_NewObject(ctx);
    JS_SetConstructor(ctx, base, baseProto);
    JS_FreeValue(ctx, baseProto);
    m_engine->setProperty(m_engine->globalObject(), "__JZWidgetObject", base);   // 消费 base
}

bool JZJSWidgetModule::setOp(JSValueConst jsThis, void* object)
{
    JSContext* ctx = m_engine->context();
    if (!m_opClassId)
    {
        JS_ThrowInternalError(ctx, "控件 op 槽未初始化(引擎没有装配 widget 模块)");
        return false;
    }
    if (!object)
    {
        JS_ThrowInternalError(ctx, "setOp: 底层对象为空");
        return false;
    }

    // 已经填过: 先放掉旧守卫(重复 init 时不留泄漏)
    if (void* old = JS_GetOpaque(jsThis, m_opClassId))
        delete (JZObjectPointer*)old;

    JZObjectPointer* guard = new JZObjectPointer(((JZObject*)object)->guardPointer());
    if (JS_SetOpaque(jsThis, guard) != 0)
    {
        // 走到这里说明实例不是基构造器造的(普通 JS 对象没有 opaque 可写)
        delete guard;
        JS_ThrowTypeError(ctx, "setOp: 这个 JS 对象没有 opaque 槽(实例必须由 __JZWidgetObject 派生出来)");
        return false;
    }
    return true;
}

void* JZJSWidgetModule::opFromThis(JSContext* ctx, JSValueConst jsThis, const char* what)
{
    if (!m_opClassId)
    {
        JS_ThrowInternalError(ctx, "%s: 控件 op 槽未初始化", what ? what : "opFromThis");
        return nullptr;
    }

    JSClassID class_id = 0;
    void* opaque = JS_GetAnyOpaque(jsThis, &class_id);
    if (class_id != m_opClassId || !opaque)
    {
        JS_ThrowTypeError(ctx, "%s: this 不是控件对象", what ? what : "opFromThis");
        return nullptr;
    }

    JZObject* object = ((JZObjectPointer*)opaque)->object();
    if (!object)
    {
        // 底层对象已经被删掉(父控件析构 / 引擎关闭 / 显式 delete) —— 给明确异常, 不读悬垂内存
        JS_ThrowTypeError(ctx, "%s: 对象已被销毁", what ? what : "opFromThis");
        return nullptr;
    }
    return object;
}

bool JZJSWidgetModule::widgetValue(JSValueConst v, JZWidget*& out)
{
    out = nullptr;
    if (!m_opClassId || JS_IsNull(v) || JS_IsUndefined(v))
        return false;

    JSClassID class_id = 0;
    void* opaque = JS_GetAnyOpaque(v, &class_id);
    if (class_id != m_opClassId || !opaque)
        return false;

    JZObject* object = ((JZObjectPointer*)opaque)->object();
    if (!object)
        return false;

    out = dynamic_cast<JZWidget*>(object);
    return out != nullptr;
}

bool JZJSWidgetModule::argWidget(JSContext* ctx, JSValueConst v, JZWidget*& out, const char* what, bool allowNull)
{
    if (allowNull && (JS_IsNull(v) || JS_IsUndefined(v)))
    {
        out = nullptr;
        return true;
    }

    JZWidget* w = dynamic_cast<JZWidget*>(static_cast<JZObject*>(opFromThis(ctx, v, what)));
    if (!w)
    {
        out = nullptr;
        return false;   // opFromThis 已抛异常(或类型不符)
    }

    out = w;
    return true;
}

void JZJSWidgetModule::defNative(JSValue proto, const char* name, JSCFunction* fn, int length)
{
    JSContext* ctx = m_engine->context();
    JS_DefinePropertyValueStr(ctx, proto, name, JS_NewCFunction(ctx, fn, name, length),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}

// 把整数常量挂到某个已存在的全局类(构造器)上 —— JS 侧 `JZWidget.CustomContextMenu` 取到。
// 与 JZChart 的类型常量同一套路: def() 注册的是**原型方法**, 静态值只能挂在构造器本身。
// 类还不存在(没加载 widget/dist/classes.js)就静默跳过: 没有类也就没有可写 JS。
void JZJSWidgetModule::defClassInt(const char* className, const char* name, int value)
{
    JSContext* ctx = m_engine->context();
    JSValue cls = JS_GetPropertyStr(ctx, m_engine->globalObject(), className);
    if (!JS_IsObject(cls))
    {
        JS_FreeValue(ctx, cls);
        return;
    }
    JS_SetPropertyStr(ctx, cls, name, JS_NewInt32(ctx, value));
    JS_FreeValue(ctx, cls);
}

// ============================================================================
// 透明控件类: 原生方法(def) / 初始化入口(setInit) / 类查找
// ============================================================================
// 与 jsbind::ClassBind 的区别(为什么不能用那套): ClassBind 会把类登记进引擎 —— m_typeMap、
// m_funcList、形参类型表、magic 分发全都要插一脚, 而控件族要对引擎**透明**。
// 这里只借它的"从函数签名推形参类型"那几个 traits, 生成的原生函数完全归本模块所有。

namespace
{
// JS 类名 -> 蛇形: "JZMainWindow" -> "main_window"(生成 init_c_<蛇形> 的名字用)
std::string jzSnakeName(const std::string& jsName)
{
    std::string in = jsName;
    if (in.size() > 2 && in.compare(0, 2, "JZ") == 0)
        in = in.substr(2);

    std::string out;
    for (size_t i = 0; i < in.size(); i++)
    {
        char c = in[i];
        if (c >= 'A' && c <= 'Z')
        {
            if (!out.empty())
                out += '_';
            out += (char)(c - 'A' + 'a');
        }
        else
        {
            out += c;
        }
    }
    return out;
}
}   // namespace

namespace
{
// 方法调用的 trampoline: magic = 模块方法表下标。argv[0] 约定为 this(与引擎 magic 分发一致),
// 所以 CFunction::call(ctx, argv) 可以直接用 —— 实参转换与旧路径**同一份实现**。
JSValue jzMethodTrampoline(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv, int magic)
{
    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    JZJSWidgetModule* module = engine ? engine->widgetModule() : nullptr;
    if (!module)
        return JS_ThrowInternalError(ctx, "控件方法: 引擎没有装配 widget 模块");
    return module->callTransparentMethod(magic, thisVal, argc, argv);
}

// init_c_xxx 的 trampoline: 造底层对象 + 填 op 槽 + 装事件过滤器 + connect 信号
JSValue jzInitTrampoline(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv, int magic)
{
    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    JZJSWidgetModule* module = engine ? engine->widgetModule() : nullptr;
    if (!module)
        return JS_ThrowInternalError(ctx, "init_c_xxx: 引擎没有装配 widget 模块");
    return module->callTransparentInit(magic, thisVal, argc, argv);
}
}   // namespace

void JZJSWidgetModule::mountTransparentClass(const CClassDefine& define)
{
    JSContext* ctx = m_engine->context();

    // TS 类层定义的类(定义在 classes.ts, 由 widget/dist/index.js 装上)
    JSValue cls = JS_GetPropertyStr(ctx, m_engine->globalObject(), define.name.c_str());
    if (!JS_IsFunction(ctx, cls))
    {
        JS_FreeValue(ctx, cls);
        LOG_W(std::string("透明控件类: 全局类不存在 ") + define.name + "(TS 类层没装? 见 loadWidgetClasses)");
        return;
    }
    JSValue proto = JS_GetPropertyStr(ctx, cls, "prototype");

    // 原型方法: 每个 CFunction 挂成一个原生 quickjs 函数(不进引擎的 m_funcList/形参表)
    for (size_t i = 0; i < define.functions.size(); i++)
    {
        std::shared_ptr<CFunction> func = define.functions[i];
        m_methods.push_back(func);   // 保活: func->name 是 quickjs 用的裸指针宿主

        int magic = (int)m_methods.size() - 1;
        JS_DefinePropertyValueStr(ctx, proto, func->name.c_str(),
                                  JS_NewCFunctionMagic(ctx, jzMethodTrampoline, func->name.c_str(),
                                                       (int)func->params.size(), JS_CFUNC_generic_magic, magic),
                                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    }

    // 裸 C 函数(ClassBind::def(name, JSCFunction*)): 原样挂(this_val/argv 自己处理)
    for (size_t i = 0; i < define.rawFunctions.size(); i++)
    {
        CRawFunction raw = define.rawFunctions[i];
        JS_DefinePropertyValueStr(ctx, proto, raw.name.c_str(),
                                  JS_NewCFunction(ctx, raw.function, raw.name.c_str(), raw.length),
                                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        m_rawMethods.push_back(raw);
    }

    // 初始化入口: 全局 init_c_<蛇形类名>(this, ...args) —— 复用 define.create(就是 js_JZxxx_ctor)
    if (define.create)
    {
        JzInitEntry entry;
        entry.className = define.name;
        entry.create = define.create;

        int magic = (int)m_inits.size();
        m_inits.push_back(entry);

        std::string fnName = "init_c_" + jzSnakeName(define.name);
        JS_SetPropertyStr(ctx, m_engine->globalObject(), fnName.c_str(),
                          JS_NewCFunctionMagic(ctx, jzInitTrampoline, fnName.c_str(), 0, JS_CFUNC_generic_magic, magic));
    }

    JS_FreeValue(ctx, proto);
    JS_FreeValue(ctx, cls);
}

JSValue JZJSWidgetModule::callTransparentMethod(int index, JSValueConst thisVal, int argc, JSValueConst* argv)
{
    JSContext* ctx = m_engine->context();
    if (index < 0 || index >= (int)m_methods.size())
        return JS_ThrowInternalError(ctx, "控件方法: 方法表下标越界");

    CFunction* func = m_methods[index].get();

    // 形参个数只做**越界保护**(少了会读到 argv 之外)。
    // 注意 params[0] 是 this(成员函数), 真实形参个数要减 1。
    int need = (int)func->params.size() - 1;
    if (argc != need)
        return JS_ThrowTypeError(ctx, "%s: 需要 %d 个参数", func->name.c_str(), need);

    // 宿主(形参表第 0 项, 即 this)也按对象指针校验: `JZLabel.prototype.setText.call(123, 'x')`
    // 这类"拿方法去调一个非控件"要被立刻拦下, 而不是静默什么都不做。
    {
        const CFunctionParam& p0 = func->params[0];
        int t0 = (p0.type == JSType_none) ? m_engine->ctypeidToType(p0.ctypeid) : p0.type;
        if (t0 == JSType_objectPointer && !m_engine->checkFunctionArg(p0, 0, thisVal))
            return JS_EXCEPTION;
    }

    // 对象形参的**形参层校验**: 声明成 `JZObjectPointer*` 的形参必须是"活着的对象指针" ——
    // 交给引擎的 checkFunctionArg(它按 JSType_objectPointer 判 opaque + 守卫存活)。这样
    // "传了个数字/字符串/普通对象当控件"在进方法体之前就被拦下, 而不是拖到里面当指针用。
    // 标量/字符串/值类型形参仍不在这里校验(类型契约在 types/*.d.ts)。
    for (int i = 0; i < argc; i++)
    {
        const CFunctionParam& p = func->params[i + 1];
        int arg_type = (p.type == JSType_none) ? m_engine->ctypeidToType(p.ctypeid) : p.type;
        if (arg_type == JSType_objectPointer && !m_engine->checkFunctionArg(p, i, argv[i]))
            return JS_EXCEPTION;
    }

    // CFunction 的约定: argv[0] = this, 后面是实参
    std::vector<JSValue> args;
    args.reserve(argc + 1);
    args.push_back(thisVal);
    for (int i = 0; i < argc; i++)
        args.push_back(argv[i]);

    // C++ 异常 -> JS 异常。⚠ 这层以前在 JZJSEngine::call 里(引擎的 magic 分发路径), 我们是直接调
    // CFunction, 所以必须自己兜: 不然 lambda 里 throw(如 JZChart 的越界检查)会穿出 C 边界 terminate。
    try
    {
        return func->call(ctx, args.data());
    }
    catch (std::exception& e)
    {
        return JS_ThrowInternalError(ctx, "%s", e.what());
    }
}

JSValue JZJSWidgetModule::callTransparentInit(int index, JSValueConst /*thisVal*/, int argc, JSValueConst* argv)
{
    JSContext* ctx = m_engine->context();
    if (index < 0 || index >= (int)m_inits.size())
        return JS_ThrowInternalError(ctx, "init_c_xxx: 登记下标越界");

    // JS 侧写法是**普通函数调用**: `init_c_label(this, ...args)` —— 控件对象在 argv[0],
    // 真正的构造实参从 argv[1] 开始(this 是 undefined, 不能用)。
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "init_c_%s: 需要控件对象参数(JS 侧写法 init_c_xxx(this, ...args))",
                                 m_inits[index].className.c_str());

    JSValueConst jsThis = argv[0];
    JzInitEntry& entry = m_inits[index];

    // 复用既有 js_JZxxx_ctor 的实参解析(它按 JS 实参形态分派到 C++ 构造重载)
    m_engine->setCurrentArgs(argc - 1, argv + 1);
    void* ptr = entry.create(m_engine, argc - 1, argv + 1);
    if (!ptr)
        return JS_EXCEPTION;   // 构造器已经是"抛 JS 异常 + 返回 nullptr"的口径

    if (!setOp(jsThis, ptr))
    {
        delete (JZObject*)ptr;   // op 槽没写进去就别把对象留在那儿(guard 已抛异常)
        return JS_EXCEPTION;
    }

    JZObject* object = (JZObject*)ptr;

    // 宿主句柄: 事件/信号派发时都要用它回到**这个 JS 对象**上现读属性(onPaint / onClicked ...)
    attachJsValue(object, jsThis);

    JZWidget* widget = dynamic_cast<JZWidget*>(object);
    if (widget)
    {
        widget->installEventFilter(this);   // 事件转发: 模块当过滤器(幂等)

        // 构造时直接带父(`new JZLabel('x', win)` / `new JZPushButton('ok', win)` ...):
        // C++ 侧的父子关系已经由构造器建好了, 但 JS 便利层(tree.ts)不知道 —— 而**保活**与
        // parent()/children() 都靠 JS 侧那张登记表。所以这里回调一下便利层的钩子补登记
        // (钩子由 tree.ts 装载时挂到 globalThis.__jzWidgetLink; 没装就跳过, 不影响构造)。
        if (JZWidget* parent = widget->parentWidget())
        {
            JSValue parentJs = jsValueOf(parent);
            if (!JS_IsUndefined(parentJs))
            {
                JSValue hook = JS_GetPropertyStr(ctx, m_engine->globalObject(), "__jzWidgetLink");
                if (JS_IsFunction(ctx, hook))
                {
                    JSValue args[2] = { parentJs, jsThis };
                    JSValue ret = m_engine->callJs(hook, JS_UNDEFINED, 2, args);
                    if (JS_IsException(ret))
                    {
                        JSValue err = JS_GetException(ctx);
                        LOG_W("构造带父时补 JS 侧登记失败: " + fromJSValue<std::string>(ctx, err));
                        JS_FreeValue(ctx, err);
                    }
                    JS_FreeValue(ctx, ret);
                }
                JS_FreeValue(ctx, hook);
            }
        }
    }

    connectSignals(jsThis, object);         // 该类(及其祖先类)登记过的信号: 连一次
    return JS_UNDEFINED;
}

bool JZJSWidgetModule::loadWidgetClasses()
{
    if (!m_engine)
        return false;

    // 只加载 **类层**(classes.js: 定义全局控件类)—— 不含 tree.ts 那层 JS 便利层,
    // 因为便利层要在"方法挂好之后"才装(它会在装载时把原生方法缓存下来, 见 loadWidgetShell)。
    // 类层/便利层都是"引擎内部库"而不是用户入口: runModuleFile 会把第一个成功加载的模块记成
    // appFile, 所以这里先存后恢复(宿主已 setAppFile 的场合不能顶掉)。
    std::string appFile = m_engine->appFilePath();
    bool ok = m_engine->runModuleFile("widget/dist/classes.js");
    if (!appFile.empty())
        m_engine->setAppFile(appFile);

    if (!ok)
        return false;

    JSValue classes = JS_GetPropertyStr(m_engine->context(), m_engine->globalObject(), "__jzWidgetClasses");
    bool loaded = JS_IsObject(classes);
    JS_FreeValue(m_engine->context(), classes);
    return loaded;
}

bool JZJSWidgetModule::loadWidgetShell()
{
    if (!m_engine)
        return false;

    // 包入口(index.js): 先 import './classes'(已在模块缓存里, 空跑)再装 tree.ts 那层 JS 便利层
    // (对象树登记/保活/弱引用父)。**必须在方法挂完之后** —— tree.ts 装载时会缓存
    // JZWidget.prototype.setParentWidget 等原生方法, 挂晚了它拿到的就是 undefined。
    std::string appFile = m_engine->appFilePath();
    bool ok = m_engine->runModuleFile("widget/dist/index.js");
    if (!appFile.empty())
        m_engine->setAppFile(appFile);
    return ok;
}

// ---------- 宿主句柄 js_value(见 JZJSWidgetModule.h 的 JZJSHandle) ----------

// 对象上挂"JS 对象"用的属性名。用动态属性而不是给控件类型加成员: 绑定层状态不进 JZCore/JZWidget,
// 一个对象一份, 随对象析构自然消失。
static const char* JZ_JS_VALUE_PROP = "js_value";

JSValue JZJSWidgetModule::jsValueOf(JZObject* object)
{
    if (!object)
        return JS_UNDEFINED;

    JZVariant v = object->property(JZ_JS_VALUE_PROP);
    if (v.typeId() != jzMetaTypeId<JZJSHandle>())
        return JS_UNDEFINED;

    JZJSHandle handle = v.value<JZJSHandle>();
    return handle.isNull() ? JS_UNDEFINED : handle.value();   // 借用
}

void JZJSWidgetModule::attachJsValue(JZObject* object, JSValueConst value)
{
    if (!m_engine || !object || JS_IsUndefined(value) || JS_IsNull(value))
        return;

    // 覆盖旧的即可(同一对象的宿主句柄只有一份)
    object->setProperty(JZ_JS_VALUE_PROP, JZVariant::fromValue(JZJSHandle(m_engine, value)));
}

JSValue JZJSWidgetModule::widgetJsView(JZWidget* widget)
{
    if (!m_engine || !widget)
        return JS_NULL;

    JSValue js = jsValueOf(widget);       // 借用
    if (JS_IsUndefined(js))
        return JS_NULL;                   // 从没被 JS 拿到过: 造不出"同一份", 交给调用方决定怎么报
    return JS_DupValue(m_engine->context(), js);
}

// ---------- 列表业务数据槽: 数据存 JS 侧(见 setItemData 的说明) ----------

// 属性名(挂在列表的 JS 包装对象上)。用 __jz 前缀, 避开应用自己的动态属性。
static const char* JZ_ITEM_DATA_PROP = "__jzItemData";

// 取业务数据数组。create=true 时没有就造一个挂上去。
// 返回**新引用**(调用方负责 JS_FreeValue); 拿不到列表的 JS 包装时返回 JS_UNDEFINED(借用常量)。
JSValue JZJSWidgetModule::itemDataArray(JZListWidget* l, bool create)
{
    JSContext* ctx = m_engine ? m_engine->context() : nullptr;
    if (!ctx || !l)
        return JS_UNDEFINED;

    JSValue obj = jsValueOf(l);   // 借用
    if (JS_IsUndefined(obj))
        return JS_UNDEFINED;

    JSValue arr = JS_GetPropertyStr(ctx, obj, JZ_ITEM_DATA_PROP);
    if (JS_IsUndefined(arr) && create)
    {
        JS_FreeValue(ctx, arr);
        arr = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, obj, JZ_ITEM_DATA_PROP, JS_DupValue(ctx, arr));   // 属性自带一份
    }
    return arr;
}

// 项增删时同步数据数组, 保持"槽跟着项走"的语义(与以前"数据挂在 C++ 项上"一致):
//   kind 0 = 在第 index 项前插入  -> splice(index, 0, undefined)(后面整体后移, 新项无数据)
//   kind 1 = 摘掉第 index 项      -> splice(index, 1)
//   kind 2 = 清空                 -> length = 0
void JZJSWidgetModule::syncItemData(JZListWidget* l, int kind, int index)
{
    JSContext* ctx = m_engine ? m_engine->context() : nullptr;
    if (!ctx || !l)
        return;

    JSValue arr = itemDataArray(l, false);
    if (JS_IsUndefined(arr))
        return;   // 从没存过业务数据: 没什么可同步的

    if (kind == 2)
    {
        JS_SetPropertyStr(ctx, arr, "length", JS_NewInt32(ctx, 0));
    }
    else
    {
        JSValue splice = JS_GetPropertyStr(ctx, arr, "splice");
        if (JS_IsFunction(ctx, splice))
        {
            JSValue args[3];
            int argc = 2;
            args[0] = JS_NewInt32(ctx, index);
            args[1] = JS_NewInt32(ctx, (kind == 0) ? 0 : 1);
            if (kind == 0)
            {
                args[2] = JS_UNDEFINED;
                argc = 3;
            }

            JSValue ret = JS_Call(ctx, splice, arr, argc, args);
            if (JS_IsException(ret))
            {
                // 内部同步失败不该影响调用方: 把挂起异常取走丢掉
                JSValue e = JS_GetException(ctx);
                JS_FreeValue(ctx, e);
            }
            JS_FreeValue(ctx, ret);
            for (int i = 0; i < argc; i++)
                JS_FreeValue(ctx, args[i]);
        }
        JS_FreeValue(ctx, splice);
    }
    JS_FreeValue(ctx, arr);
}

void JZJSWidgetModule::bindJsEventFilter(JZWidget* widget, JSValueConst value)
{
    if (!m_engine || !widget)
        return;

    attachJsValue(widget, value);
    widget->installEventFilter(this);   // 事件转发: 模块自己当过滤器(幂等, 重复安装被忽略)
}

void JZJSWidgetModule::onJsObjectCreated(int type, JSValueConst object, void* ptr)
{
    if (!m_engine || !ptr)
        return;

    // ⚠ ptr 未必是 JZObject: 值类型(JZRect/JZColor/JZImage...)的 create 返回的是它自己的对象。
    //   只有 "指针语义 + JZObject 派生" 的类(guardedOpaque)才能当 JZObject 用。
    if (m_engine->isGuardedType(type))
    {
        JZObject* object_ = (JZObject*)ptr;
        attachJsValue(object_, object);   // 事件/信号都要靠它回到 JS 对象上读回调

        JZWidget* widget = dynamic_cast<JZWidget*>(object_);
        if (widget)
            widget->installEventFilter(this);

        // 该类(及其祖先类)登记过的信号: 连一次就行, 之后改 JS 属性即可
        connectSignals(object, object_);
        return;
    }

    // 非 JZObject 的类: 只关心**借给 JS 看的视图** —— 目前只有绘制派发里的 JZPainter:
    // 底层对象归模块(m_painter, 派发结束时回收), 所以这份 JS 视图要记下来, 收尾时作废
    // (disarmOpaque), 否则 JS 的终结器会去 delete 模块的对象 + 派发后还能读到悬垂指针。
    if (m_painter && type == m_engine->ctypeidToType(typeid(JZPainter).name()))
    {
        if (!JS_IsUndefined(m_paintView))
            JS_FreeValue(m_engine->context(), m_paintView);
        m_paintView = JS_DupValue(m_engine->context(), object);   // 派发期间强引用: 别让它被 GC 掉
    }
}

JZWidget* JZJSWidgetModule::takeWidgetFromJs(JSValue value)
{
    // 控件族对引擎透明 ⇒ 不能用引擎的类型检查(它的类型表里没有控件类), 走 op 槽:
    // opFromThis 对"不是控件/已销毁"已经抛了 JS 异常并返回 nullptr。
    return dynamic_cast<JZWidget*>(
        static_cast<JZObject*>(opFromThis(m_engine->context(), value, "需要 JZWidget(或其子类)")));
}

// ---------- JS 事件转发(模块自己当过滤器; 见 JZJSWidgetModule.h) ----------
// 回调就是 JS 对象上的普通属性(`w.onPaint = fn`), C++ 侧不存一份 —— 派发时现读。

const char* JZJSWidgetModule::eventSlotName(int slot)
{
    switch (slot)
    {
    case Slot_paint:        return "onPaint";
    case Slot_mousePress:   return "onMousePress";
    case Slot_mouseRelease: return "onMouseRelease";
    case Slot_mouseMove:    return "onMouseMove";
    case Slot_wheel:        return "onWheel";
    case Slot_enter:        return "onEnter";
    case Slot_leave:        return "onLeave";
    case Slot_keyPress:     return "onKeyPress";
    case Slot_keyRelease:   return "onKeyRelease";
    case Slot_focusIn:      return "onFocusIn";
    case Slot_focusOut:     return "onFocusOut";
    case Slot_show:         return "onShow";
    case Slot_hide:         return "onHide";
    case Slot_resize:       return "onResize";
    default:                return "";
    }
}

// 从一个 JS 对象上按属性名取回调。返回**新引用**(调用方负责 JS_FreeValue)。
JSValue JZJSWidgetModule::slotValue(JSValue value, const char* slot_name)
{
    if (!m_engine || !slot_name || !JS_IsObject(value))
        return JS_UNDEFINED;

    return JS_GetPropertyStr(m_engine->context(), value, slot_name);
}

bool JZJSWidgetModule::hasCallback(JZObject* object, const char* name)
{
    if (!m_engine || !object)
        return false;

    JSValue jsObj = jsValueOf(object);
    if (JS_IsUndefined(jsObj))
        return false;

    JSValue fn = slotValue(jsObj, name);
    bool isFn = JS_IsFunction(m_engine->context(), fn);
    JS_FreeValue(m_engine->context(), fn);
    return isFn;
}

// 以控件的 JS 对象为 this 调回调; 事件对象包成**借用视图**(事件是栈对象, 只在本次派发期间有效)
template <class E>
void JZJSWidgetModule::forwardJs(JSValue jsObj, const char* name, E* event)
{
    JSContext* ctx = m_engine->context();

    // 事件对象 -> JS 视图。owned=false(JZEvent 派生自 JZObject, 所以是守卫句柄):
    // 事件是 C++ 栈对象、由派发方 delete, JS 不负责释放; 派发结束后事件析构, 守卫变空 ——
    // JS 侧若还留着这份事件对象, 再访问它会抛"对象已被销毁"而不是读悬垂内存。
    JSValue eventVal = m_engine->toJsValueFromPtr(event);
    if (JS_IsNull(eventVal))
        return;

    std::vector<JZJSValuePtr> params;
    params.push_back(m_engine->makeValuePtr(eventVal));   // 接管所有权

    // this = 控件**自己那个 JS 对象**(回调里 this、以及 self() 拿到的都是它)
    m_engine->callMemberFunction(name, JZJSValuePtr(ctx, JS_DupValue(ctx, jsObj)), params);

    // 视图作废: 事件是 C++ 栈对象, 不能让它将来按守卫去 delete(disarm 后 opaque 为空,
    // 守卫也已释放)。JS 侧还留着的那个事件对象再从 C++ 拿对象会抛异常, 不会 UAF。
    m_engine->disarmOpaque(eventVal);
}

template <class E>
bool JZJSWidgetModule::forwardIf(JSValue jsObj, const char* name, E* event)
{
    JSContext* ctx = m_engine->context();
    JSValue fn = slotValue(jsObj, name);
    bool isFn = JS_IsFunction(ctx, fn);
    JS_FreeValue(ctx, fn);

    if (!isFn)
        return false;   // 没设回调(或不是函数) -> 放行走原生实现

    forwardJs(jsObj, name, event);
    return true;
}

bool JZJSWidgetModule::eventFilter(JZObject* watched, JZEvent* event)
{
    if (!m_engine || !watched || !event)
        return false;

    // 禁用态先于一切: 被 setEnabled(false) 的控件(含祖先禁用, 见 JZWidget::isEnabled)对输入类
    // 事件一律**吞掉** —— 既不转给 JS 回调, 也不放行走原生实现。这一步必须在过滤器里做:
    // 过滤器比 JZWidget::event 更靠前(见 JZObject::sendEvent 的链), 只在 event() 里拦会漏掉
    // JS 回调(它正是在这里被转发的)。
    if (event->type() == JZEvent_mouse || event->type() == JZEvent_key || event->type() == JZEvent_ime)
    {
        if (!static_cast<JZWidget*>(watched)->isEnabled())
            return true;
    }

    // 转发目标 = 控件自己那个 JS 对象(挂在动态属性 js_value 上)。取不到(纯 C++ 建出来的控件、
    // 或 JS 对象已经回收)就直接放行走原生实现。
    JSValue jsObj = jsValueOf(watched);
    if (JS_IsUndefined(jsObj))
        return false;

    // 事件类型 -> 回调属性名(见 eventSlotName)。返回 true = 已转给 JS, 事件被拦下(不再走原生实现)。
    switch (event->type())
    {
    case JZEvent_paint:
        // JS 侧画笔(new JZPainter(this))的生命周期 = 本次 onPaint 派发(对应 C++ 的栈对象
        // `JZPainter p(this)`): 出作用域立即回收, 让绘制流的 Push/Pop 同帧配平。
        if (!hasCallback(watched, eventSlotName(Slot_paint)))
            return false;
        {
            beginPainter((JZWidget*)watched);
            forwardJs(jsObj, eventSlotName(Slot_paint), static_cast<JZPainterEvent*>(event));
            endPainter();
        }
        return true;
    case JZEvent_mouse:
    {
        JZMouseEvent* me = static_cast<JZMouseEvent*>(event);
        switch (me->mouseType())
        {
        case JZMouseEvent::Press:   return forwardIf(jsObj, eventSlotName(Slot_mousePress), me);
        case JZMouseEvent::Release: return forwardIf(jsObj, eventSlotName(Slot_mouseRelease), me);
        case JZMouseEvent::Move:    return forwardIf(jsObj, eventSlotName(Slot_mouseMove), me);
        case JZMouseEvent::Wheel:   return forwardIf(jsObj, eventSlotName(Slot_wheel), me);
        }
        return false;
    }
    case JZEvent_enter:    return forwardIf(jsObj, eventSlotName(Slot_enter), event);
    case JZEvent_leave:    return forwardIf(jsObj, eventSlotName(Slot_leave), event);
    case JZEvent_key:
    {
        JZKeyEvent* ke = static_cast<JZKeyEvent*>(event);
        return ke->isPress() ? forwardIf(jsObj, eventSlotName(Slot_keyPress), ke)
                             : forwardIf(jsObj, eventSlotName(Slot_keyRelease), ke);
    }
    case JZEvent_focusIn:  return forwardIf(jsObj, eventSlotName(Slot_focusIn), event);
    case JZEvent_focusOut: return forwardIf(jsObj, eventSlotName(Slot_focusOut), event);
    case JZEvent_show:     return forwardIf(jsObj, eventSlotName(Slot_show), static_cast<JZShowEvent*>(event));
    case JZEvent_hide:     return forwardIf(jsObj, eventSlotName(Slot_hide), static_cast<JZHideEvent*>(event));
    case JZEvent_resize:   return forwardIf(jsObj, eventSlotName(Slot_resize), static_cast<JZResizeEvent*>(event));
    default:
        return false;
    }
}

// ---------- 信号 -> 回调 ----------
// 信号在 JS 侧就是**属性**(`btn.onClicked = fn`): 对象诞生时把它 connect 一次(见 connectSignals),
// 连接里的 lambda 每次去对象上现读属性 —— 属性不是函数(没设/被清空)就跳过, 不需要断开连接。

JSValue JZJSWidgetModule::callJsCallback(JSValueConst fn, JZObject* owner, int argc, JSValueConst* argv)
{
    JSContext* ctx = m_engine->context();

    // 把"发信号的对象"压成当前 this: 回调写成箭头函数时也能用 self() 认出自己是谁。
    // 用对象**自己那个 JS 对象**(宿主句柄), 不现造视图 —— 现造的视图被 GC 时会按守卫把对象删掉。
    JSValue selfVal = jsValueOf(owner);
    JZJSEngine::SelfScope self_scope(m_engine, selfVal);

    // 调用期间自己持一份: 回调里若把同一个信号属性重设/清空(槽里的旧值会被 release),
    // 正在执行的这个函数对象仍然是活的 —— 否则就是"调用中把自己释放掉"的 UAF。
    JSValue held = JS_DupValue(ctx, fn);

    // 回调里抛出的异常不能跨 quickjs 的 C 调用栈传回去(信号是 C++ 侧同步 emit 的),
    // 按引擎既有口径: 交给 reportUncaughtError 并入错误链(sigError/sigLog)。
    JSValue ret = m_engine->callJs(held, JS_UNDEFINED, argc, argv);
    JS_FreeValue(ctx, held);

    if (JS_IsException(ret))
    {
        JSValue ex = JS_GetException(ctx);
        const char* s = JS_ToCString(ctx, ex);
        m_engine->reportUncaughtError(s ? s : "JS 回调抛出异常(错误信息不可读)");
        if (s)
            JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, ex);
        return JS_EXCEPTION;
    }
    return ret;
}

// 信号连接: 每个(对象, 信号)在对象诞生时连一次(见 connectSignals), lambda 每次去 JS 对象上
// **现读属性**并调用。所以"清空"信号(`btn.onClicked = null`)不需要断开连接 —— 读到的不是函数就跳过。
// 接收者写模块自己: 模块先于对象销毁时, 连接会被自动摘掉(不会留下捕获 owner 的悬挂槽)。
void JZJSWidgetModule::ensureConnected(JZObject* owner, JZSigVoid& sig, const char* name)
{
    if (!m_engine || !owner || !name)
        return;

    connect(&sig, this, [this, owner, name]() {
        JSValue cb = slotValue(jsValueOf(owner), name);   // 新引用
        if (JS_IsFunction(m_engine->context(), cb))
            callJsCallback(cb, owner, 0, nullptr);
        JS_FreeValue(m_engine->context(), cb);
    });
}

void JZJSWidgetModule::ensureConnected(JZObject* owner, JZSigBool& sig, const char* name)
{
    if (!m_engine || !owner || !name)
        return;

    connect(&sig, this, [this, owner, name](bool v) {
        JSValue cb = slotValue(jsValueOf(owner), name);
        if (JS_IsFunction(m_engine->context(), cb))
        {
            JSValue a = toJSValue(m_engine->context(), v);
            callJsCallback(cb, owner, 1, &a);
            JS_FreeValue(m_engine->context(), a);
        }
        JS_FreeValue(m_engine->context(), cb);
    });
}

void JZJSWidgetModule::ensureConnected(JZObject* owner, JZSigInt& sig, const char* name)
{
    if (!m_engine || !owner || !name)
        return;

    connect(&sig, this, [this, owner, name](int v) {
        JSValue cb = slotValue(jsValueOf(owner), name);
        if (JS_IsFunction(m_engine->context(), cb))
        {
            JSValue a = toJSValue(m_engine->context(), v);
            callJsCallback(cb, owner, 1, &a);
            JS_FreeValue(m_engine->context(), a);
        }
        JS_FreeValue(m_engine->context(), cb);
    });
}

void JZJSWidgetModule::ensureConnected(JZObject* owner, JZSigString& sig, const char* name)
{
    if (!m_engine || !owner || !name)
        return;

    connect(&sig, this, [this, owner, name](const std::string& v) {
        JSValue cb = slotValue(jsValueOf(owner), name);
        if (JS_IsFunction(m_engine->context(), cb))
        {
            JSValue a = toJSValue(m_engine->context(), v);
            callJsCallback(cb, owner, 1, &a);
            JS_FreeValue(m_engine->context(), a);
        }
        JS_FreeValue(m_engine->context(), cb);
    });
}

void JZJSWidgetModule::ensureConnected(JZObject* owner, JZSigDouble& sig, const char* name)
{
    if (!m_engine || !owner || !name)
        return;

    connect(&sig, this, [this, owner, name](double v) {
        JSValue cb = slotValue(jsValueOf(owner), name);
        if (JS_IsFunction(m_engine->context(), cb))
        {
            JSValue a = toJSValue(m_engine->context(), v);
            callJsCallback(cb, owner, 1, &a);
            JS_FreeValue(m_engine->context(), a);
        }
        JS_FreeValue(m_engine->context(), cb);
    });
}

void JZJSWidgetModule::ensureConnected(JZObject* owner, JZSigDateTime& sig, const char* name)
{
    if (!m_engine || !owner || !name)
        return;

    // 日期直接给内建 Date(不是 C++ 值类型包装): JS 侧 `d.getTime()` / `d instanceof Date` /
    // 兼容层的 asDate() 都按这个形状认, 与移植过来的 Qt 代码写法一致。
    connect(&sig, this, [this, owner, name](const JZDateTime& v) {
        JSValue cb = slotValue(jsValueOf(owner), name);
        if (JS_IsFunction(m_engine->context(), cb))
        {
            JSValue a = JS_NewDate(m_engine->context(), (double)v.toMSecsSinceEpoch());
            callJsCallback(cb, owner, 1, &a);
            JS_FreeValue(m_engine->context(), a);
        }
        JS_FreeValue(m_engine->context(), cb);
    });
}

void JZJSWidgetModule::ensureConnected(JZObject* owner, JZSignal<int, int>& sig, const char* name)
{
    if (!m_engine || !owner || !name)
        return;

    connect(&sig, this, [this, owner, name](int a1, int a2) {
        JSValue cb = slotValue(jsValueOf(owner), name);
        if (JS_IsFunction(m_engine->context(), cb))
        {
            JSValue args[2] = { toJSValue(m_engine->context(), a1), toJSValue(m_engine->context(), a2) };
            callJsCallback(cb, owner, 2, args);
            JS_FreeValue(m_engine->context(), args[0]);
            JS_FreeValue(m_engine->context(), args[1]);
        }
        JS_FreeValue(m_engine->context(), cb);
    });
}

// ---------- 信号属性的注册样板 ----------
// 信号就是 JS 对象上的**普通属性**(`btn.onClicked = fn`): 不注册 C 访问器, C++ 侧也不存副本。
// 这里只做一件事 —— 把"该类有这个信号"记进登记表; 对象创建时按类层次全部 connect 一次
// (见 connectSignals), 之后每次 emit 都去对象上现读属性来调。

void JZJSWidgetModule::connectSignals(JSValue jsThis, JZObject* object)
{
    if (!m_engine || !object || JS_IsUndefined(jsThis) || JS_IsNull(jsThis))
        return;

    JSContext* ctx = m_engine->context();

    // 按 JS 类名 + JS_IsInstanceOf 认类: 对象的祖先类登记过什么信号就全连上
    // (控件族的类层次在 TS/JS 侧, 引擎的类型表对它们是空的 —— 这里不能用引擎那套判定)。
    for (auto& kv : m_signalBinders)
    {
        JSValue ctor = JS_GetPropertyStr(ctx, m_engine->globalObject(), kv.first.c_str());
        if (!JS_IsFunction(ctx, ctor))
        {
            JS_FreeValue(ctx, ctor);
            continue;
        }
        int isInstance = JS_IsInstanceOf(ctx, jsThis, ctor);
        JS_FreeValue(ctx, ctor);
        if (!isInstance)
            continue;

        for (auto& binder : kv.second)
            binder(object, this);
    }
}

// 登记一个信号: JS 类名 -> [ (对象, 模块) => 把该信号 connect 到"读 JS 属性再调"的 lambda ]
template <class S, class Sig>
void JZJSWidgetModule::registSignal(const char* jsClassName, const char* name, Sig S::* sig)
{
    if (!jsClassName || !name)
        return;

    m_signalBinders[jsClassName].push_back(
        [name, sig](JZObject* object, JZJSWidgetModule* module) {
            // 类的继承关系与 C++ 一致 ⇒ 该对象一定派生自信号声明的那个类 S, 按 S* 取成员
            S* obj = static_cast<S*>(object);
            module->ensureConnected(obj, obj->*sig, name);
        });
}

template <class C, class S, class Sig>
void JZJSWidgetModule::defSignalProperty(jsbind::ClassBind<C>& bind, const char* name, Sig S::* sig)
{
    // 键用 ClassBind 注册的 JS 类名(registClass 会把类挂到全局同名对象上)
    registSignal(bind.classDefine().name.c_str(), name, sig);
}

// ---------- 模块生命周期 ----------

JZJSWidgetModule::JZJSWidgetModule() {}
JZJSWidgetModule::~JZJSWidgetModule() { deinitEngine(); }

void JZJSWidgetModule::initEngine(JZJSEngine* engine)
{
    m_engine = engine;
    
    // 带 func_data 的 C 函数(setParentWidget)要拿到本模块: 把指针做成一个"整数句柄"备好,
    // 注册时注入(func_data 只在 C 侧可见, JS 侧读不到)。用整数而不是包装对象:
    // 不必为它注册 JS 类/造对象, 也没有所有权与 GC 问题(见 JZJSWidgetModule.h 的 m_widgetData)。
    if (engine)
    {
        m_widgetData = JS_NewInt64(engine->context(), (int64_t)(intptr_t)this);
        engine->setWidgetModule(this);   // 各个 JS 构造函数要拿它登记控件表(见 widgetModule())

        // "JS 对象先存在": create 造出 JS 对象后把宿主句柄记到 C++ 对象上(见 onJsObjectCreated)
        engine->setObjectCreatedCallback(
            [this](int type, JSValueConst object, void* ptr) { onJsObjectCreated(type, object, ptr); });
    }

    // 透明控件类: ① 建 op 槽所需的基构造器/自定义 class; ② 加载 TS 类层(定义全局控件类)。
    // **顺序很讲究**(分三段):
    //   1) 引擎注册的值类型/事件/布局/画笔...(控件族以外的东西) —— TS 类层里的 tree.ts 会往
    //      JZLayout 等**引擎注册类**的原型上装方法, 所以那些类必须先存在;
    //   2) 加载 TS 类层(定义全局控件类 + 装对象树方法);
    //   3) registWidgets/registDialogs 把 C++ 方法挂到这些 TS 类的原型上。
    if (engine)
        initWidgetOpClass();

    registGeometry();
    registColor();
    registImage();
    registEvents();
    registSettings();
    registApp();
    registLayouts();
    registPens();
    registMatrix();
    registPainter();

    if (engine)
    {
        if (!loadWidgetClasses())
            LOG_W("控件类层没装上(widget/dist/index.js): 控件族不可用 —— 检查 libraryDir / 是否跑过 tsc");
    }

    registWidgets();
    registDialogs();
    registViews();       // 容器/视图类控件(JZListWidget/JZStackedWidget/... , 同为透明类)

    if (engine)
    {
        if (!loadWidgetShell())
            LOG_W("控件包入口没装上(widget/dist/index.js): JS 侧对象树/保活(tree.ts)不可用");
    }

    registUiLoader();
}

void JZJSWidgetModule::deinitEngine()
{    
    m_widgetData = JS_UNDEFINED;   // 整数句柄, 无引用计数, 直接丢

    if (m_engine)
    {
        m_engine->setWidgetModule(nullptr);
        // 引擎要先于模块销毁的场合(宿主反序收尾)不能让回调继续指着本模块
        m_engine->setObjectCreatedCallback(nullptr);
        m_engine->setWidgetOpClassId(0);
    }

    endPainter();   // 把可能还挂着的绘制句柄/视图收掉(m_paintView 是强引用)

    // 换引擎/重装时登记表要清掉(ctypeid 是同一进程共享的, 留着会重复登记)
    m_signalBinders.clear();

    // 透明类的表(方法/裸函数/初始化入口)也是每次装配重建
    m_methods.clear();
    m_rawMethods.clear();
    m_inits.clear();

    // op class 随 runtime 走(基构造器是挂在全局对象上的, 随 ctx 一起销毁)
    m_opClassId = 0;

    m_engine = nullptr;   
}

JZPainter* JZJSWidgetModule::createPainter(JSContext* ctx, int argc, JSValueConst* argv)
{// create 约定: 参数不合法时"设 JS 异常 + 返回 nullptr"(不抛 C++ 异常),
    // JZJSEngine::createValue 见到空指针就把该异常交给 JS。
    if (argc < 1)
    {
        JS_ThrowTypeError(ctx, "new JZPainter(target): 需要 JZWidget 或 JZImage 参数");
        return nullptr;
    }

    // 离屏绘制: 目标是图像 —— JZPainter 自持图层, 析构时合成回 image, 不受绘制派发约束。
    if (m_engine->checkType(argv[0], "JZImage"))   // 校验在前(构造参数没有类型表, 得自己先校验)
    {
        JZImage* img = fromJSValue<JZImage*>(ctx, argv[0]);
        return new JZPainter(*img);
    }
    // 控件目标: 控件族对引擎透明, 所以不用 checkType("JZWidget"), 走 op 槽判定
    else if (JZWidget* target = dynamic_cast<JZWidget*>(
                 static_cast<JZObject*>(opFromThis(ctx, argv[0], "new JZPainter(target)"))))
    {
        if (target != m_paintWidget)
        {
            JS_ThrowTypeError(ctx, "new JZPainter(target): 只能在目标的 onPaint 回调里创建");
            return nullptr;
        }
        if (m_painter)
        {
            // 同一次绘制内第二份画笔: 直接抛(与"一份画笔 = 一个绘制作用域"的约定一致)
            JS_ThrowTypeError(ctx, "不能创建多个JZPainter");
            return nullptr;
        }
        // 本控件这次派发的绘制句柄: 归模块所有, 派发结束(beginPainter/endPainter)时回收。
        // 交给 JS 的那份视图在 onJsObjectCreated 里登记, 收尾时作废(见 m_paintView)。
        m_painter = new JZPainter(target);
        return m_painter;
    }
    else
    {
        JS_ThrowTypeError(ctx, "new JZPainter(target): 需要 JZWidget 或 JZImage 参数");
        return nullptr;
    }
}

void JZJSWidgetModule::beginPainter(JZWidget* widget)
{
    m_paintWidget = widget;
}

void JZJSWidgetModule::endPainter()
{
    if (m_painter)
    {
        delete m_painter;
        m_painter = nullptr;
    }

    // 交给 JS 的那份视图作废: 之后按它调方法会抛"已失效", GC 时也不会去 delete 模块的对象
    if (m_engine && !JS_IsUndefined(m_paintView))
    {
        m_engine->disarmOpaque(m_paintView);
        JS_FreeValue(m_engine->context(), m_paintView);
        m_paintView = JS_UNDEFINED;
    }
    m_paintWidget = nullptr;
}

JZJSWidgetModule* JZJSWidgetModule::fromFuncData(JSContext* ctx, JSValueConst* func_data)
{
    if (!func_data)
    {
        JS_ThrowInternalError(ctx, "JZJSWidgetModule: C 函数缺少 func_data(注册时应走 defData)");
        return nullptr;
    }

    int64_t handle = 0;
    if (JS_ToInt64(ctx, &handle, func_data[0]) < 0)
        return nullptr;   // JS_ToInt64 已设异常

    if (handle == 0)
    {
        JS_ThrowInternalError(ctx, "JZJSWidgetModule: 模块指针为空(模块已销毁)");
        return nullptr;
    }
    return (JZJSWidgetModule*)(intptr_t)handle;
}

// 把类的**构造器**挂静态函数(JS 侧 `JZImage.create(...)`)。
void JZJSWidgetModule::defStaticFunction(const CClassDefine& define, const char* name, JSCFunction* fn, int length)
{
    int type = m_engine->ctypeidToType(define.ctypeid);
    if (type == JSType_none)
        return;

    JSContext* ctx = m_engine->context();
    JSValue js_class = m_engine->classConstructor(type);   // 新引用
    // JS_SetPropertyStr 消费 value, 不能再 free
    JS_SetPropertyStr(ctx, js_class, name, JS_NewCFunction(ctx, fn, name, length));
    JS_FreeValue(ctx, js_class);
}

// ---------- 几何类型 ----------

void JZJSWidgetModule::registGeometry()
{
    jsbind::ClassBind<JZPoint> bind_point("JZPoint");
    bind_point.def("setX", [](JZPoint* p, int x) { p->setX(x); });
    bind_point.def("setY", [](JZPoint* p, int y) { p->setY(y); });
    bind_point.def("setPoint", [](JZPoint* p, int x, int y) { p->setPoint(x, y); });
    bind_point.def("x", [](JZPoint* p) { return p->x(); });
    bind_point.def("y", [](JZPoint* p) { return p->y(); });
    bind_point.def("isNull", [](JZPoint* p) { return p->isNull(); });
    // 值相等: JS/TS 没有运算符重载 ⇒ `a === b` 用的是包装身份(每次取都是新包装, 恒 false),
    // 要比"坐标一样"必须显式调用。其余值类型(JZSize/JZRect/JZColor/JZFont)同理。
    bind_point.def("equal", [](JZPoint* p, const JZPoint& o) { return *p == o; });
    bind_point.setCreate(js_JZPoint_ctor);
    m_engine->registClass(bind_point.classDefine());

    jsbind::ClassBind<JZSize> bind_size("JZSize");
    bind_size.def("setWidth", [](JZSize* s, int w) { s->setWidth(w); });
    bind_size.def("setHeight", [](JZSize* s, int h) { s->setHeight(h); });
    bind_size.def("width", [](JZSize* s) { return s->width(); });
    bind_size.def("height", [](JZSize* s) { return s->height(); });
    bind_size.def("isEmpty", [](JZSize* s) { return s->isEmpty(); });
    bind_size.def("isValid", [](JZSize* s) { return s->isValid(); });
    bind_size.def("equal", [](JZSize* s, const JZSize& o) { return *s == o; });
    bind_size.setCreate(js_JZSize_ctor);
    m_engine->registClass(bind_size.classDefine());

    jsbind::ClassBind<JZRect> bind_rect("JZRect");
    bind_rect.def("setX", [](JZRect* r, int x) { r->setX(x); });
    bind_rect.def("setY", [](JZRect* r, int y) { r->setY(y); });
    bind_rect.def("setWidth", [](JZRect* r, int w) { r->setWidth(w); });
    bind_rect.def("setHeight", [](JZRect* r, int h) { r->setHeight(h); });
    bind_rect.def("setRect", [](JZRect* r, int x, int y, int w, int h) { r->setRect(x, y, w, h); });
    bind_rect.def("x", [](JZRect* r) { return r->x(); });
    bind_rect.def("y", [](JZRect* r) { return r->y(); });
    bind_rect.def("width", [](JZRect* r) { return r->width(); });
    bind_rect.def("height", [](JZRect* r) { return r->height(); });
    bind_rect.def("left", [](JZRect* r) { return r->left(); });
    bind_rect.def("top", [](JZRect* r) { return r->top(); });
    bind_rect.def("right", [](JZRect* r) { return r->right(); });
    bind_rect.def("bottom", [](JZRect* r) { return r->bottom(); });
    bind_rect.def("center", [](JZRect* r) { return r->center(); });
    bind_rect.def("size", [](JZRect* r) { return r->size(); });
    bind_rect.def("topLeft", [](JZRect* r) { return r->topLeft(); });
    bind_rect.def("bottomRight", [](JZRect* r) { return r->bottomRight(); });
    bind_rect.def("contains", [](JZRect* r, int x, int y) { return r->contains(x, y); });
    bind_rect.def("intersects", [](JZRect* r, const JZRect& o) { return r->intersects(o); });
    bind_rect.def("intersect", [](JZRect* r, const JZRect& o) { return r->intersect(o); });
    bind_rect.def("united", [](JZRect* r, const JZRect& o) { return r->united(o); });
    bind_rect.def("isEmpty", [](JZRect* r) { return r->isEmpty(); });
    bind_rect.def("equal", [](JZRect* r, const JZRect& o) { return *r == o; });
    bind_rect.setCreate(js_JZRect_ctor);
    m_engine->registClass(bind_rect.classDefine());
}

// ---------- 颜色 ----------

void JZJSWidgetModule::registColor()
{
    jsbind::ClassBind<JZColor> bind_color("JZColor");
    bind_color.def("red", [](JZColor* c) { return (int)c->red(); });
    bind_color.def("green", [](JZColor* c) { return (int)c->green(); });
    bind_color.def("blue", [](JZColor* c) { return (int)c->blue(); });
    bind_color.def("alpha", [](JZColor* c) { return (int)c->alpha(); });
    bind_color.def("setRgb", [](JZColor* c, int r, int g, int b) { c->setRgb((uint8_t)r, (uint8_t)g, (uint8_t)b); });
    bind_color.def("setRgba", [](JZColor* c, int r, int g, int b, int a) { c->setRgba((uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a); });
    bind_color.def("equal", [](JZColor* c, const JZColor& o) { return *c == o; });
    bind_color.setCreate(js_JZColor_ctor);
    m_engine->registClass(bind_color.classDefine());
}

// ---------- 图像 ----------

// JZImage.create(w, h) —— 新建纯透明（alpha=0）画布, 是 JS 侧唯一的"造图"入口:
//   离屏绘制: const img = JZImage.create(200, 100); const p = new JZPainter(img); ... p.end();
//   ImageLabel: new JZImageLabel().setImage(img)
// 挂成**构造器上的静态函数**(见 defStaticFunction 的说明): def() 只注册原型方法, JS 里
// `JZImage.create(...)` 取的是函数对象自身的属性, 拿不到原型方法。
static JSValue js_JZImageCreate(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    if (argc != 2)
        return JS_ThrowTypeError(ctx, "JZImage.create(w, h) 需要 2 个参数");

    // 构造式入口没有形参类型表, 转换前必须自己校验(fromJSValue 不校验会致命断言)
    if (!engine->checkType(argv[0], JSType_int) || !engine->checkType(argv[1], JSType_int))
        return JS_ThrowTypeError(ctx, "JZImage.create(w, h): 宽高需要数字");

    int w = fromJSValue<int>(ctx, argv[0]);
    int h = fromJSValue<int>(ctx, argv[1]);
    return toJSValue(ctx, JZImage::create(w, h));
}

void JZJSWidgetModule::registImage()
{
    jsbind::ClassBind<JZImage> bind_img("JZImage");
    bind_img.def("width", [](JZImage* img) { return img->width(); });
    bind_img.def("height", [](JZImage* img) { return img->height(); });
    bind_img.def("isNull", [](JZImage* img) { return img->isNull(); });
    bind_img.def("save", [](JZImage* img, const std::string& file) { return img->save(file); });
    bind_img.def("scaled", [](JZImage* img, int w, int h) { return img->scaled(w, h); });
    bind_img.setCreate(js_JZImage_ctor);
    m_engine->registClass(bind_img.classDefine());

    defStaticFunction(bind_img.classDefine(), "create", js_JZImageCreate, 2);
}

// ---------- 事件 (按类注册) ----------

void JZJSWidgetModule::registEvents()
{
    // 基类方法只在 JZEvent 上注册一次, 子类经 JS 原型链继承 (ClassBind<Sub>(name, bind_event))。
    jsbind::ClassBind<JZEvent> bind_event("JZEvent");
    bind_event.def("type", [](JZEvent* e) { return e->type(); });
    bind_event.def("accept", [](JZEvent* e) { e->accept(); });
    bind_event.def("ignore", [](JZEvent* e) { e->ignore(); });
    bind_event.def("isAccepted", [](JZEvent* e) { return e->isAccepted(); });
    m_engine->registClass(bind_event.classDefine());

    jsbind::ClassBind<JZMouseEvent> bind_mouse("JZMouseEvent", bind_event);
    bind_mouse.def("localX", [](JZMouseEvent* e) { return e->localPos().x(); });
    bind_mouse.def("localY", [](JZMouseEvent* e) { return e->localPos().y(); });
    bind_mouse.def("globalX", [](JZMouseEvent* e) { return e->globalPos().x(); });
    bind_mouse.def("globalY", [](JZMouseEvent* e) { return e->globalPos().y(); });
    bind_mouse.def("x", [](JZMouseEvent* e) { return e->localPos().x(); });
    bind_mouse.def("y", [](JZMouseEvent* e) { return e->localPos().y(); });
    bind_mouse.def("button", [](JZMouseEvent* e) { return (int)e->button(); });
    bind_mouse.def("wheelDelta", [](JZMouseEvent* e) { return e->wheelDelta(); });
    bind_mouse.def("modifiers", [](JZMouseEvent* e) { return (int)e->modifiers(); });
    m_engine->registClass(bind_mouse.classDefine());

    jsbind::ClassBind<JZKeyEvent> bind_key("JZKeyEvent", bind_event);
    bind_key.def("key", [](JZKeyEvent* e) { return (int)e->key(); });
    bind_key.def("modifiers", [](JZKeyEvent* e) { return (int)e->modifiers(); });
    bind_key.def("text", [](JZKeyEvent* e) { return e->text(); });
    bind_key.def("isRepeat", [](JZKeyEvent* e) { return e->isRepeat(); });
    bind_key.def("isPress", [](JZKeyEvent* e) { return e->isPress(); });
    bind_key.def("shift", [](JZKeyEvent* e) { return e->shift(); });
    bind_key.def("ctrl", [](JZKeyEvent* e) { return e->ctrl(); });
    bind_key.def("alt", [](JZKeyEvent* e) { return e->alt(); });
    m_engine->registClass(bind_key.classDefine());

    // JZResizeEvent 现为 JZEvent 派生(见 JZResizeEvent.h), 故与其它事件一样挂到 JZEvent 原型下
    jsbind::ClassBind<JZResizeEvent> bind_resize("JZResizeEvent", bind_event);
    bind_resize.def("width", [](JZResizeEvent* e) { return e->size().width(); });
    bind_resize.def("height", [](JZResizeEvent* e) { return e->size().height(); });
    bind_resize.def("oldWidth", [](JZResizeEvent* e) { return e->oldSize().width(); });
    bind_resize.def("oldHeight", [](JZResizeEvent* e) { return e->oldSize().height(); });
    bind_resize.def("size", [](JZResizeEvent* e) { return e->size(); });
    bind_resize.def("oldSize", [](JZResizeEvent* e) { return e->oldSize(); });
    m_engine->registClass(bind_resize.classDefine());    

    jsbind::ClassBind<JZPainterEvent> bind_paint("JZPainterEvent", bind_event);
    bind_paint.def("rect", [](JZPainterEvent* e) { return e->rect(); });
    // 注意: 不暴露 painter() —— 事件里的 JZPainter 由渲染调度器持有、仅在本次派发期间有效,
    // 做成 JS 返回值就会在派发结束后悬垂(返回值又不能"用完就 clear")。
    m_engine->registClass(bind_paint.classDefine());

    jsbind::ClassBind<JZShowEvent> bind_show("JZShowEvent", bind_event);
    m_engine->registClass(bind_show.classDefine());

    jsbind::ClassBind<JZHideEvent> bind_hide("JZHideEvent", bind_event);
    m_engine->registClass(bind_hide.classDefine());
    
    jsbind::ClassBind<JZCloseEvent> bind_close("JZCloseEvent", bind_event);
    m_engine->registClass(bind_close.classDefine());    

    jsbind::ClassBind<JZTimerEvent> bind_timer("JZTimerEvent", bind_event);
    bind_timer.def("id", [](JZTimerEvent* e) { return e->id(); });
    m_engine->registClass(bind_timer.classDefine());
}

// 所有权翻转(/Transfer/ 与 /TransferBack/)用 JZJSWidget.h 里的 jzSetCppOwner:
// 那边一处实现, 这里和 C 函数共用(包装类型随类层次不同, 必须经 JSPointerBase 多态走)。

// 校验"是控件(或其子类)"并取裸指针。出错抛 std::runtime_error
// (由 JZJSEngine::call 的 try/catch 转成 JS 异常)。
// 控件族对引擎透明 ⇒ 不用引擎的类型检查, 走 op 槽(见 widgetValue)。
static JZWidget* jsWidgetArg(JSContext* ctx, JSValueConst value, const char* what)
{
    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    JZJSWidgetModule* module = engine ? engine->widgetModule() : nullptr;
    if (!module)
        throw std::runtime_error(std::string(what) + ": 引擎没有装配 widget 模块");

    JZWidget* w = nullptr;
    if (!module->widgetValue(value, w))
        throw std::runtime_error(std::string(what) + ": 需要 JZWidget(或其子类)");
    return w;
}

// JZVariant -> JS 值(`w.property(name)` 用)。JZVariant 存取是**精确类型**(toXxx 会断言类型匹配),
// 所以这里按 typeId 逐个精确取值; 覆盖: 空/标量/字符串/字节串/数组/对象/JS 值(注册的 metatype)。
// 其它 metatype(C++ 侧自定义类型)没有通用转换, 明确抛异常 —— 不静默返回 null 丢信息。
// ---------- 对话框 (模态) ----------
// 模态循环 = JZWin32Thread::execModal(可重入, 与主循环共用轮体): exec() 期间照常跑 JS 回调与定时器,
// 所以 exec() 返回后"世界可能已经变了"(与 Qt 同款约定)。同一个对象嵌套 exec() 由 JZDialog 自己拒绝
// (警告 + 返回 -1, 见 JZDialog.cpp); 不同对话框互相嵌套是支持的(各自独立栈帧)。
// 注册方式: JZDialog 是 JZObject -> 按类注册; JZMessageBox / JZFileDialog 只有静态接口 ->
// 挂成全局对象(与 uiLoader / jzWidget 同套路)。
// 注: 这些入口是 quickjs C 函数(不走 JZJSEngine::call), 所以**不能抛 C++ 异常** —— 出错一律
// JS_ThrowTypeError + return JS_EXCEPTION(与 js_JZImageCreate 同一约定)。

// 解析对话框的 (parent?, caption?, dir?, filter?): 允许省略尾部参数; 类型不对时已设 JS 异常。
static bool jzJsParseDialogArgs(JSContext* ctx, int argc, JSValueConst* argv, const char* what,
                                JZWidget*& parent, std::string& caption, std::string& dir, std::string& filter,
                                std::string* directoryOnly = nullptr)
{
    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    if (!engine)
        return false;

    if (argc > 0 && !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0]))
    {
        JZJSWidgetModule* module = engine->widgetModule();
        if (!module || !module->widgetValue(argv[0], parent))
        {
            JS_ThrowTypeError(ctx, "%s: 第 1 个参数需要 JZWidget(或其子类), 也可以传 null", what);
            return false;
        }
    }

    std::string* outs[3] = { &caption, &dir, directoryOnly ? directoryOnly : &filter };
    for (int i = 0; i < 3; i++)
    {
        int index = i + 1;
        if (argc <= index || JS_IsNull(argv[index]) || JS_IsUndefined(argv[index]))
            continue;
        if (!engine->checkType(argv[index], JSType_string))
        {
            JS_ThrowTypeError(ctx, "%s: 第 %d 个参数需要字符串", what, index + 1);
            return false;
        }
        *outs[i] = fromJSValue<std::string>(ctx, argv[index]);
    }
    return true;
}

// JZMessageBox.information/warning/question(parent, title, text) -> 1=Accepted / 0=Rejected
// 语义取 Qt5 的静态接口(直接给结果), 但不是把 C++ 那个"返回对话框对象"照搬: 对话框是一次性的,
// exec() 结束后 deleteLater 收尾, 不把句柄交给 JS(省掉所有权问题)。
static JSValue jsJZMessageBoxRun(JSContext* ctx, int argc, JSValueConst* argv, int kind)
{
    const char* what = "JZMessageBox.information/warning/question(parent, title, text)";
    if (argc != 3)
        return JS_ThrowTypeError(ctx, "%s: 需要 3 个参数", what);

    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    if (!engine)
        return JS_EXCEPTION;

    JZJSWidgetModule* module = engine->widgetModule();
    JZWidget* parent = nullptr;
    if (!module || !module->widgetValue(argv[0], parent))
        return JS_ThrowTypeError(ctx, "%s: 第 1 个参数需要 JZWidget(或其子类)", what);
    if (!engine->checkType(argv[1], JSType_string) || !engine->checkType(argv[2], JSType_string))
        return JS_ThrowTypeError(ctx, "%s: title / text 需要字符串", what);

    std::string title = fromJSValue<std::string>(ctx, argv[1]);
    std::string text = fromJSValue<std::string>(ctx, argv[2]);

    JZMessageBox* box = (kind == 2) ? JZMessageBox::question(parent, title, text)
                        : (kind == 1) ? JZMessageBox::warning(parent, title, text)
                                      : JZMessageBox::information(parent, title, text);
    int result = box->exec();
    box->deleteLater();   // 延后删除: 模态循环里不立刻析构
    return JS_NewInt32(ctx, result);
}

static JSValue jsJZMessageBoxInformation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{ return jsJZMessageBoxRun(ctx, argc, argv, 0); }
static JSValue jsJZMessageBoxWarning(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{ return jsJZMessageBoxRun(ctx, argc, argv, 1); }
static JSValue jsJZMessageBoxQuestion(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{ return jsJZMessageBoxRun(ctx, argc, argv, 2); }

// 文件对话框: 直接转发 JZFileDialog 的静态接口(内部是原生模态对话框, 期间照常跑 JS 回调/定时器)。
// 取消 / 失败 -> null(单个) 或 [](多个) —— 比 C++ 的空串更好判。
static JSValue jsJZFileDialogGetOpenFileName(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    const char* what = "JZFileDialog.getOpenFileName(parent?, caption?, dir?, filter?)";
    JZWidget* parent = nullptr;
    std::string caption, dir, filter;
    if (!jzJsParseDialogArgs(ctx, argc, argv, what, parent, caption, dir, filter))
        return JS_EXCEPTION;
    std::string path = JZFileDialog::getOpenFileName(parent, caption, dir, filter);
    return path.empty() ? JS_NULL : JS_NewString(ctx, path.c_str());
}

static JSValue jsJZFileDialogGetOpenFileNames(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    const char* what = "JZFileDialog.getOpenFileNames(parent?, caption?, dir?, filter?)";
    JZWidget* parent = nullptr;
    std::string caption, dir, filter;
    if (!jzJsParseDialogArgs(ctx, argc, argv, what, parent, caption, dir, filter))
        return JS_EXCEPTION;

    std::vector<std::string> files = JZFileDialog::getOpenFileNames(parent, caption, dir, filter);
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < files.size(); i++)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewString(ctx, files[i].c_str()));
    return arr;
}

static JSValue jsJZFileDialogGetSaveFileName(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    const char* what = "JZFileDialog.getSaveFileName(parent?, caption?, dir?, filter?)";
    JZWidget* parent = nullptr;
    std::string caption, dir, filter;
    if (!jzJsParseDialogArgs(ctx, argc, argv, what, parent, caption, dir, filter))
        return JS_EXCEPTION;
    std::string path = JZFileDialog::getSaveFileName(parent, caption, dir, filter);
    return path.empty() ? JS_NULL : JS_NewString(ctx, path.c_str());
}

static JSValue jsJZFileDialogGetExistingDirectory(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    const char* what = "JZFileDialog.getExistingDirectory(parent?, caption?, dir?)";
    JZWidget* parent = nullptr;
    std::string caption, dir, filter;
    if (!jzJsParseDialogArgs(ctx, argc, argv, what, parent, caption, dir, filter, &dir))
        return JS_EXCEPTION;
    std::string path = JZFileDialog::getExistingDirectory(parent, caption, dir);
    return path.empty() ? JS_NULL : JS_NewString(ctx, path.c_str());
}

// ---------- 配置 (JZSettings, INI) ----------
// 对应 Qt 的 QSettings: 打开一个 ini 文件 -> beginGroup/setValue/getValue/sync。
// 只绑"不需要默认值参数"的形式(getValue(name) 返回空串 / getInt(name) 返回 0), 因为引擎的 def
// 要求形参个数精确匹配 —— 要默认值就在 JS 里写 `s.getValue('x') || 'def'`。
void JZJSWidgetModule::registSettings()
{
    jsbind::ClassBind<JZSettings> bind_set("JZSettings");
    bind_set.def("open", [](JZSettings* s, const std::string& file) { return s->open(file); });
    bind_set.def("sync", [](JZSettings* s) { s->sync(); });
    bind_set.def("beginGroup", [](JZSettings* s, const std::string& g) { return s->beginGroup(g); });
    bind_set.def("endGroup", [](JZSettings* s) { s->endGroup(); });
    bind_set.def("hasGroup", [](JZSettings* s, const std::string& g) { return s->hasGroup(g); });
    bind_set.def("hasValue", [](JZSettings* s, const std::string& n) { return s->hasValue(n); });
    bind_set.def("setValue", [](JZSettings* s, const std::string& n, const std::string& v) { s->setValue(n, v); });
    bind_set.def("getValue", [](JZSettings* s, const std::string& n) { return s->getValue(n); });
    bind_set.def("setBool", [](JZSettings* s, const std::string& n, bool v) { s->setBool(n, v); });
    bind_set.def("getBool", [](JZSettings* s, const std::string& n) { return s->getBool(n); });
    bind_set.def("setInt", [](JZSettings* s, const std::string& n, int v) { s->setInt(n, v); });
    bind_set.def("getInt", [](JZSettings* s, const std::string& n) { return s->getInt(n); });
    bind_set.def("setDouble", [](JZSettings* s, const std::string& n, double v) { s->setDouble(n, v); });
    bind_set.def("getDouble", [](JZSettings* s, const std::string& n) { return s->getDouble(n); });
    m_engine->registClass(bind_set.classDefine());
}

void JZJSWidgetModule::registDialogs()
{
    // 基类绑定: 只为拿 superCtypeid(与 registViews 同一套路; JZWidget 已在 registWidgets 里注册)
    jsbind::ClassBind<JZWidget> bind_w("JZWidget");

    // ---- JZDialog: 自定义模态对话框 ----
    //   const dlg = new JZDialog();
    //   dlg.setParentWidget(owner);      // 可选: 有 owner 才会"模态禁用 owner" + 相对它居中
    //   ... 往 dlg 里放控件 ...
    //   const r = dlg.exec();            // 阻塞(嵌套事件循环); 1=Accepted, 0=Rejected, -1=递归调用被拒
    jsbind::ClassBind<JZDialog> bind_dlg("JZDialog", bind_w);
    
    bind_dlg.def("exec", [this](JZObjectPointer* d) {
        JZDialog* dlg = widgetFromThis<JZDialog>(d, "exec");
        return dlg ? dlg->exec() : -1;
    });
    bind_dlg.def("accept", [this](JZObjectPointer* d) {
        if (JZDialog* dlg = widgetFromThis<JZDialog>(d, "accept"))
            dlg->accept();
    });
    bind_dlg.def("reject", [this](JZObjectPointer* d) {
        if (JZDialog* dlg = widgetFromThis<JZDialog>(d, "reject"))
            dlg->reject();
    });
    bind_dlg.def("result", [this](JZObjectPointer* d) {
        JZDialog* dlg = widgetFromThis<JZDialog>(d, "result");
        return dlg ? (int)dlg->result() : 0;
    });
    // 对话框信号: 属性形(单发/覆盖式); onFinished 带 result(int)
    defSignalProperty(bind_dlg, "onAccepted", &JZDialog::sigAccepted);
    defSignalProperty(bind_dlg, "onRejected", &JZDialog::sigRejected);
    defSignalProperty(bind_dlg, "onFinished", &JZDialog::sigFinished);
    mountTransparentClass(bind_dlg.classDefine());

    // ---- 全局 JZMessageBox / JZFileDialog(只有静态接口, 挂成普通对象) ----
    JSContext* ctx = m_engine->context();

    JSValue messageBox = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, messageBox, "information", JS_NewCFunction(ctx, jsJZMessageBoxInformation, "information", 3));
    JS_SetPropertyStr(ctx, messageBox, "warning", JS_NewCFunction(ctx, jsJZMessageBoxWarning, "warning", 3));
    JS_SetPropertyStr(ctx, messageBox, "question", JS_NewCFunction(ctx, jsJZMessageBoxQuestion, "question", 3));
    m_engine->setProperty(m_engine->globalObject(), "JZMessageBox", messageBox);

    JSValue fileDialog = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, fileDialog, "getOpenFileName", JS_NewCFunction(ctx, jsJZFileDialogGetOpenFileName, "getOpenFileName", 4));
    JS_SetPropertyStr(ctx, fileDialog, "getOpenFileNames", JS_NewCFunction(ctx, jsJZFileDialogGetOpenFileNames, "getOpenFileNames", 4));
    JS_SetPropertyStr(ctx, fileDialog, "getSaveFileName", JS_NewCFunction(ctx, jsJZFileDialogGetSaveFileName, "getSaveFileName", 4));
    JS_SetPropertyStr(ctx, fileDialog, "getExistingDirectory", JS_NewCFunction(ctx, jsJZFileDialogGetExistingDirectory, "getExistingDirectory", 3));
    m_engine->setProperty(m_engine->globalObject(), "JZFileDialog", fileDialog);
}

void JZJSWidgetModule::registWidgets()
{
    // 基类 JZWidget —— 裸类对象。
    // 注意: 事件回调(onPaint / onMousePress ...)**不需要在这里注册属性** —— 它们就是 JS 对象上的
    // 普通属性, 由事件过滤器在派发时现读(见 eventFilter / slotValue)。信号(onClicked ...)也只登记
    // "该类有这个信号"(见各 bind_xxx 的 defSignalProperty), 连接在对象创建时建立。
    jsbind::ClassBind<JZWidget> bind_w("JZWidget");

    // 延后删除 —— 嵌套模态循环里删控件的正规做法: 事件派发期间(尤其 exec() 的模态循环里)直接把控件
    // delete 掉, 会让外层正在派发的事件对象悬垂; deleteLater 排到本轮事件处理完之后。
    bind_w.def("deleteLater", [this](JZObjectPointer* w) {
        if (JZWidget* self = widgetFromThis<JZWidget>(w, "deleteLater"))
            self->deleteLater();
    });

    bind_w.def("move", [this](JZObjectPointer* w, int x, int y) {
        if (JZWidget* self = widgetFromThis<JZWidget>(w, "move"))
            self->move(x, y);
    });
    bind_w.def("x", [this](JZObjectPointer* w) {
        JZWidget* self = widgetFromThis<JZWidget>(w, "x");
        return self ? self->x() : 0;
    });
    bind_w.def("y", [this](JZObjectPointer* w) {
        JZWidget* self = widgetFromThis<JZWidget>(w, "y");
        return self ? self->y() : 0;
    });
    bind_w.def("width", [this](JZObjectPointer* w) {
        JZWidget* self = widgetFromThis<JZWidget>(w, "width");
        return self ? self->width() : 0;
    });
    bind_w.def("height", [this](JZObjectPointer* w) {
        JZWidget* self = widgetFromThis<JZWidget>(w, "height");
        return self ? self->height() : 0;
    });
    bind_w.def("resize", [this](JZObjectPointer* w, int w_, int h) {
        if (JZWidget* self = widgetFromThis<JZWidget>(w, "resize"))
            self->resize(w_, h);
    });
    bind_w.def("setFixedSize", [this](JZObjectPointer* w, int w_, int h) {
        if (JZWidget* self = widgetFromThis<JZWidget>(w, "setFixedSize"))
            self->setFixedSize(w_, h);
    });
    bind_w.def("setFixedWidth", [this](JZObjectPointer* w, int v) {
        if (JZWidget* self = widgetFromThis<JZWidget>(w, "setFixedWidth"))
            self->setFixedWidth(v);
    });
    bind_w.def("setFixedHeight", [this](JZObjectPointer* w, int v) {
        if (JZWidget* self = widgetFromThis<JZWidget>(w, "setFixedHeight"))
            self->setFixedHeight(v);
    });
    bind_w.def("setMinimumSize", [this](JZObjectPointer* w, int w_, int h) {
        if (JZWidget* self = widgetFromThis<JZWidget>(w, "setMinimumSize"))
            self->setMinimumSize(w_, h);
    });
    bind_w.def("setMaximumSize", [this](JZObjectPointer* w, int w_, int h) {
        if (JZWidget* self = widgetFromThis<JZWidget>(w, "setMaximumSize"))
            self->setMaximumSize(w_, h);
    });
    bind_w.def("setGeometry", [this](JZObjectPointer* w, int x, int y, int w_, int h) {
        if (JZWidget* self = widgetFromThis<JZWidget>(w, "setGeometry"))
            self->setGeometry(x, y, w_, h);
    });
    bind_w.def("geometry", [this](JZObjectPointer* w) {
        JZWidget* self = widgetFromThis<JZWidget>(w, "geometry");
        return self ? self->geometry() : JZRect();
    });
    // 坐标映射: C++ 里 mapTo/mapToGlobal 各有 JZPoint / JZRect 两个重载, 而 jsbind 一个名字
    // 只有一条形参表 ⇒ 形参收 JSValue 自行判型, 两个重载都暴露(传别的类型抛 TypeError)。
    //   mapTo(parent, pt)      parent 为 null 时等价于映射到屏幕(与 C++ 一致)
    //   mapToGlobal(pt)        控件局部 -> 屏幕逻辑坐标(内部已做 ×scale / ClientToScreen)
    // 典型用途: 右键菜单/弹层锚点 —— `const p = w.mapToGlobal(pt); menu.popup(win, new JZRect(p.x(), p.y(), 1, 1))`;
    // 对位对齐/命中判断用 mapTo(容器, pt)。
    auto f_mapTo = bind_w.def("mapTo", [this](JZObjectPointer* w, JZObjectPointer* parent, JSValue v) -> JSValue {
        JSContext* ctx = m_engine->context();
        JZWidget* self = widgetFromThis<JZWidget>(w, "mapTo");
        if (!self)
            return JS_EXCEPTION;
        // parent = null/undefined ⇒ 映射到屏幕坐标(形参层按 allowNull 放行); 传非对象已被形参层拦下
        JZWidget* host = widgetFromThis<JZWidget>(parent, "mapTo");
        if (js_check_class_arg<JZPoint>(ctx, v))
            return toJSValue(ctx, self->mapTo(host, fromJSValue<JZPoint>(ctx, v)));
        if (js_check_class_arg<JZRect>(ctx, v))
            return toJSValue(ctx, self->mapTo(host, fromJSValue<JZRect>(ctx, v)));
        return JS_ThrowTypeError(ctx, "JZWidget.mapTo(parent, pt): pt 需要 JZPoint(或 JZRect)");
    });
    f_mapTo->params[1].allowNull = true;      // parent = null ⇒ 映射到屏幕坐标

    bind_w.def("mapToGlobal", [this](JZObjectPointer* w, JSValue v) -> JSValue {
        JSContext* ctx = m_engine->context();
        JZWidget* self = widgetFromThis<JZWidget>(w, "mapToGlobal");
        if (!self)
            return JS_EXCEPTION;
        if (js_check_class_arg<JZPoint>(ctx, v))
            return toJSValue(ctx, self->mapToGlobal(fromJSValue<JZPoint>(ctx, v)));
        if (js_check_class_arg<JZRect>(ctx, v))
            return toJSValue(ctx, self->mapToGlobal(fromJSValue<JZRect>(ctx, v)));
        return JS_ThrowTypeError(ctx, "JZWidget.mapToGlobal(pt): 需要 JZPoint(或 JZRect)");
    });
    bind_w.def("setVisible", [this](JZObjectPointer* w, bool v) {
        if (JZWidget* self = widgetFromThis<JZWidget>(w, "setVisible"))
            self->setVisible(v);
    });
    bind_w.def("isVisible", [this](JZObjectPointer* w) {
        JZWidget* self = widgetFromThis<JZWidget>(w, "isVisible");
        return self && self->isVisible();
    });
    // 使能状态(等价 Qt setEnabled/isEnabled): 禁用的控件不接收鼠标/键盘/IME 事件,
    // isEnabled() 沿父链判定(祖先禁用则自身也禁用)。语义详见 JZWidget.h。
    bind_w.def("setEnabled", [this](JZObjectPointer* w, bool on) {
        if (JZWidget* self = widgetFromThis<JZWidget>(w, "setEnabled"))
            self->setEnabled(on);
    });
    bind_w.def("isEnabled", [this](JZObjectPointer* w) {
        JZWidget* self = widgetFromThis<JZWidget>(w, "isEnabled");
        return self && self->isEnabled();
    });
    bind_w.def("show", [this](JZObjectPointer* w) {
        if (JZWidget* self = widgetFromThis<JZWidget>(w, "show"))
            self->show();
    });
    bind_w.def("hide", [this](JZObjectPointer* w) {
        if (JZWidget* self = widgetFromThis<JZWidget>(w, "hide"))
            self->hide();
    });

    // setParentWidget: parent 允许 null/undefined = 脱离父树, 其余必须是控件。
    // 脱离时把接收者翻回 JS 拥有, 挂载时翻成 C++ 拥有(见 JZJSWidget.h 所有权约定)。
    auto f_setParent = bind_w.def("setParentWidget", [this](JZObjectPointer* w, JZObjectPointer* parent) {
        JZWidget* self = widgetFromThis<JZWidget>(w, "setParentWidget");
        if (!self)
            return;
        // parent 传非对象/已销毁 -> 形参层(checkFunctionArg)已经拦下并抛异常, 走不到这里
        self->setParentWidget(widgetFromThis<JZWidget>(parent, "setParentWidget"));   // null ⇒ 脱离
    });
    f_setParent->params[1].allowNull = true;   // params[0] 是 this; parent = null/undefined ⇒ 脱离父树

    // setLayout 返回"是否被接受": C++ 是 Qt 语义(已有布局 / null -> 警告 + 忽略, 不抛),
    // JS 便利层(tree.ts)靠这个返回值决定要不要登记布局(登记错了 w.layout() 就会返回被拒的那份)。
    // 注: 走 setLayoutFromJs —— JS 侧 `new JZVBoxLayout()` 的句柄归 JS 拥有, 控件不能替它 delete
    // (否则 clearLayout()/控件析构时和 JS 的终结器双重释放)。
    // (JZLayout* 形参保持引擎转换: JZLayout 是注册类, 类型表认它)
    bind_w.def("setLayout", [this](JZObjectPointer* w, JZLayout* layout) -> bool {
        JZWidget* self = widgetFromThis<JZWidget>(w, "setLayout");
        if (!self)
            return false;
        self->setLayoutFromJs(layout);             // 虚: 容器子类(GroupBox)把它转发到内容区
        return self->appliedLayout() == layout;    // 虚: GroupBox 看的是内容区那份
    });
    bind_w.def("clearLayout", [this](JZObjectPointer* w) {
        if (JZWidget* self = widgetFromThis<JZWidget>(w, "clearLayout"))
            self->clearLayout();
    });
    bind_w.def("setFocus", [this](JZObjectPointer* w) {
        if (JZWidget* self = widgetFromThis<JZWidget>(w, "setFocus"))
            self->setFocus();
    });
    bind_w.def("hasFocus", [this](JZObjectPointer* w) {
        JZWidget* self = widgetFromThis<JZWidget>(w, "hasFocus");
        return self && self->hasFocus();
    });
    bind_w.def("update", [this](JZObjectPointer* w) {
        if (JZWidget* self = widgetFromThis<JZWidget>(w, "update"))
            self->update();
    });
    bind_w.def("setTooltip", [this](JZObjectPointer* w, const std::string& s) {
        if (JZWidget* self = widgetFromThis<JZWidget>(w, "setTooltip"))
            self->setTooltip(JZString(s.c_str()));
    });
    bind_w.def("tooltip", [this](JZObjectPointer* w) {
        JZWidget* self = widgetFromThis<JZWidget>(w, "tooltip");
        return self ? self->tooltip().toStdString() : std::string();
    });
    bind_w.def("setStyleSheet", [this](JZObjectPointer* w, const std::string& css) {
        JZWidget* self = widgetFromThis<JZWidget>(w, "setStyleSheet");
        return self ? self->setStyleSheet(css) : false;
    });
    bind_w.def("styleSheet", [this](JZObjectPointer* w) {
        JZWidget* self = widgetFromThis<JZWidget>(w, "styleSheet");
        return self ? self->styleSheet() : std::string();
    });
    // 对象名(JZObject): 样式表 #id 选择器、AI 定位、自检都靠它
    bind_w.def("setObjectName", [this](JZObjectPointer* w, const std::string& n) {
        if (JZWidget* self = widgetFromThis<JZWidget>(w, "setObjectName"))
            self->setObjectName(n);
    });
    bind_w.def("objectName", [this](JZObjectPointer* w) {
        JZWidget* self = widgetFromThis<JZWidget>(w, "objectName");
        return self ? self->objectName() : std::string();
    });
    // 样式类名(空格分隔, 供 .class 选择器)
    bind_w.def("setStyleClass", [this](JZObjectPointer* w, const std::string& cls) {
        if (JZWidget* self = widgetFromThis<JZWidget>(w, "setStyleClass"))
            self->setStyleClass(cls);
    });
    bind_w.def("styleClass", [this](JZObjectPointer* w) {
        JZWidget* self = widgetFromThis<JZWidget>(w, "styleClass");
        return self ? self->styleClass() : std::string();
    });
    // 尺寸上下限的单项设置(布局里给"固定/伸缩"留余地, 比 setMinimumSize 好用)
    bind_w.def("setMinimumWidth", [this](JZObjectPointer* w, int v) {
        if (JZWidget* self = widgetFromThis<JZWidget>(w, "setMinimumWidth"))
            self->setMinimumWidth(v);
    });
    bind_w.def("setMinimumHeight", [this](JZObjectPointer* w, int v) {
        if (JZWidget* self = widgetFromThis<JZWidget>(w, "setMinimumHeight"))
            self->setMinimumHeight(v);
    });
    bind_w.def("setMaximumWidth", [this](JZObjectPointer* w, int v) {
        if (JZWidget* self = widgetFromThis<JZWidget>(w, "setMaximumWidth"))
            self->setMaximumWidth(v);
    });
    bind_w.def("setMaximumHeight", [this](JZObjectPointer* w, int v) {
        if (JZWidget* self = widgetFromThis<JZWidget>(w, "setMaximumHeight"))
            self->setMaximumHeight(v);
    });
    // 自身矩形(控件本地坐标: x/y 为 0), 绘制回调里填背景用
    bind_w.def("rect", [this](JZObjectPointer* w) {
        JZWidget* self = widgetFromThis<JZWidget>(w, "rect");
        return self ? self->rect() : JZRect();
    });

    // ---- 右键菜单 (见 JZWidget::ContextMenuPolicy) ----
    bind_w.def("setContextMenuPolicy", [this](JZObjectPointer* w, int policy) {
        if (JZWidget* self = widgetFromThis<JZWidget>(w, "setContextMenuPolicy"))
            self->setContextMenuPolicy((JZWidget::ContextMenuPolicy)policy);
    });
    bind_w.def("contextMenuPolicy", [this](JZObjectPointer* w) {
        JZWidget* self = widgetFromThis<JZWidget>(w, "contextMenuPolicy");
        return self ? (int)self->contextMenuPolicy() : 0;
    });
    // 菜单是**借用**的(详见 JZWidget::setContextMenu): 控件持守卫不负责 delete, 也**不替它保活**
    // —— JS 侧那份被 GC 就把真菜单删了, 收到影子时 contextMenu() 会返回 null。
    // setContextMenu(null) = 解挂。(JZMenu* 形参保持引擎转换: JZMenu 是注册类)
    bind_w.def("setContextMenu", [this](JZObjectPointer* w, JZMenu* m) {
        if (JZWidget* self = widgetFromThis<JZWidget>(w, "setContextMenu"))
            self->setContextMenu(m);
    });
    bind_w.def("contextMenu", [this](JZObjectPointer* w) -> JSValue {
        JZWidget* self = widgetFromThis<JZWidget>(w, "contextMenu");
        if (!self)
            return JS_EXCEPTION;
        JZMenu* menu = self->contextMenu();
        if (!menu)
            return JS_NULL;
        JSValue v = jsValueOf(menu);
        return JS_IsUndefined(v) ? JS_NULL : JS_DupValue(m_engine->context(), v);
    });
    // 程序化触发同一套流程(AI / 测试: 坐标是本控件局部坐标)
    bind_w.def("requestContextMenu", [this](JZObjectPointer* w, int x, int y) {
        if (JZWidget* self = widgetFromThis<JZWidget>(w, "requestContextMenu"))
            self->requestContextMenu(JZPoint(x, y));
    });
    defSignalProperty(bind_w, "onCustomContextMenuRequested", &JZWidget::sigCustomContextMenuRequested);

    // 策略常量: JS 侧 `JZWidget.CustomContextMenu` / `JZLabel.NoContextMenu`(子类构造器也能取到)
    defClassInt("JZWidget", "NoContextMenu", (int)JZWidget::NoContextMenu);
    defClassInt("JZWidget", "PreventContextMenu", (int)JZWidget::PreventContextMenu);
    defClassInt("JZWidget", "DefaultContextMenu", (int)JZWidget::DefaultContextMenu);
    defClassInt("JZWidget", "ActionsContextMenu", (int)JZWidget::ActionsContextMenu);
    defClassInt("JZWidget", "CustomContextMenu", (int)JZWidget::CustomContextMenu);

    // 通用属性(setProperty/property): **不在 C++ 侧绑** —— 由 @jz/widget 的 props.ts 在 JS 侧
    // 用 WeakMap 存(见那里的说明: 值本来就是 JS 值, 绕 C++ 只会多一层转换与生命周期)。
    // C++ 的 JZObject::setProperty/property 仍在(引擎自己的宿主句柄 js_value 用它), 但不暴露给 JS。

    bind_w.setCreate(js_JZWidget_ctor);
    mountTransparentClass(bind_w.classDefine());

    auto makeWindow = [](int w, int h, const std::string& title, JZWidget* parent) {
        return new JZWindow(w, h, title, parent);
    };
    jsbind::ClassBind<JZWindow> bind_win("JZWindow", bind_w);
    bind_win.def("setTitle", [this](JZObjectPointer* w, const std::string& t) {
        if (JZWindow* win = widgetFromThis<JZWindow>(w, "setTitle"))
            win->setTitle(t);
    });
    bind_win.def("title", [this](JZObjectPointer* w) {
        JZWindow* win = widgetFromThis<JZWindow>(w, "title");
        return win ? win->title() : std::string();
    });
    bind_win.def("show", [this](JZObjectPointer* w) {
        if (JZWindow* win = widgetFromThis<JZWindow>(w, "show"))
            win->show();
    });
    bind_win.def("close", [this](JZObjectPointer* w) {
        if (JZWindow* win = widgetFromThis<JZWindow>(w, "close"))
            win->close();
    });
    // 窗口的默认纵向布局: `win.addWidget(child)` 等价 Qt 的 `win->layout()->addWidget(child)` ——
    // 窗口不再预装默认布局, 所以这里**按需补一个 VBox**。JS 侧(@jz/widget 的 tree.ts)会在外面
    // 再套一层"登记保活", 所以这里只管 C++ 侧: reparent + 进布局。
    // child 传 null 由形参层挡下(没声明 allowNull), 传非对象/已销毁同样在形参层报错。
    bind_win.def("addWidget", [this](JZObjectPointer* w, JZObjectPointer* child) {
        JZWindow* win = widgetFromThis<JZWindow>(w, "addWidget");
        if (!win)
            return;
        JZWidget* c = widgetFromThis<JZWidget>(child, "addWidget");
        if (!c)
            return;
        JZBoxLayout* box = dynamic_cast<JZBoxLayout*>(win->layout());
        if (!box)
        {
            box = new JZVBoxLayout(0, 0);
            win->setLayout(box);        // 所有权交给窗口(m_layout), ~JZWidget 里 delete
        }
        c->setParentWidget(win);        // 与 JZMainWindow::setCentralWidget 同一套路
        box->addWidget(c);
    });
    bind_win.setCreate(js_JZWindow_ctor);
    mountTransparentClass(bind_win.classDefine());    

    // JZMainWindow —— 主窗口(菜单栏 + 中央控件)。构造实参只有 (parent?),
    // 没有 w/h/title; 标题/几何走 setTitle/setGeometry。
    // uiLoader 把 <widget class="MainWindow"> 映射成 JZMainWindow, 子控件用
    // setCentralWidget(至多一个中央控件) / addMenuBar(菜单栏) 挂载, 不走通用 addWidget。
    // ⚠ 基类必须传 bind_win(JZWindow), 不是 bind_w —— JZMainWindow 派生自 JZWindow,
    // 传 bind_w 会让 setTitle/setGeometry/show/close 这些**窗口方法在它身上全部缺失**
    // (uiLoader 生成 `__w0.setTitle(...)` 直接 "not a function")。
    jsbind::ClassBind<JZMainWindow> bind_main("JZMainWindow", bind_win);    
    bind_main.def("setCentralWidget", [this](JZObjectPointer* w, JZObjectPointer* c) {
        JZMainWindow* win = widgetFromThis<JZMainWindow>(w, "setCentralWidget");
        if (!win)
            return;
        if (JZWidget* child = widgetFromThis<JZWidget>(c, "setCentralWidget"))
            win->setCentralWidget(child);
    });
    bind_main.def("addMenuBar", [this](JZObjectPointer* w, JZObjectPointer* c) {
        JZMainWindow* win = widgetFromThis<JZMainWindow>(w, "addMenuBar");
        if (!win)
            return;
        // 形参层已保证"是活的对象"; 这里再要求它确实是菜单栏(子类不符抛 TypeError)
        if (JZMenuBar* mb = widgetFromThis<JZMenuBar>(c, "addMenuBar"))
            win->addMenuBar(mb);
    });
    // 取回挂上去的那一个(没有就是 null)。**返回 JS 侧那一份包装** —— 控件族每个对象只有一份宿主句柄,
    // 现造第二份会把第一份作废(见 menus.ts 的说明), 所以走 jsValueOf 而不是现造视图。
    bind_main.def("menuBar", [this](JZObjectPointer* w) -> JSValue {
        JZMainWindow* win = widgetFromThis<JZMainWindow>(w, "menuBar");
        if (!win)
            return JS_EXCEPTION;
        JZMenuBar* mb = win->menuBar();
        if (!mb)
            return JS_NULL;
        JSValue v = jsValueOf(mb);
        return JS_IsUndefined(v) ? JS_NULL : JS_DupValue(m_engine->context(), v);
    });
    bind_main.def("centralWidget", [this](JZObjectPointer* w) -> JSValue {
        JZMainWindow* win = widgetFromThis<JZMainWindow>(w, "centralWidget");
        if (!win)
            return JS_EXCEPTION;
        JZWidget* c = win->centralWidget();
        if (!c)
            return JS_NULL;
        JSValue v = jsValueOf(c);
        return JS_IsUndefined(v) ? JS_NULL : JS_DupValue(m_engine->context(), v);
    });
    mountTransparentClass(bind_main.classDefine());

    // JZMenuBar —— 菜单栏(addMenuBar 的子控件)。仅注册构造 + 实例绑定,
    // 内部菜单项(JZMenu)暂未绑定到 JS, 由 C++ 侧继续 addMenu。
    jsbind::ClassBind<JZMenuBar> bind_menubar("JZMenuBar", bind_w);    
    // JZMenuBar: 菜单栏本身是控件; 内部菜单树由 addMenu/addAction 建(见下面的 JZMenu)。
    // addMenu/menuAt 返回 JZMenu 的**借用句柄**(对象由菜单栏持有; JS 不负责 delete) ——
    // 保活由 @jz/widget 的 menus.ts 在 JS 侧做(否则句柄被 GC 会把真菜单删掉)。
    bind_menubar.def("addMenu", [this](JZObjectPointer* w, const std::string& title) -> JSValue {
        JZMenuBar* mb = widgetFromThis<JZMenuBar>(w, "addMenu");
        return mb ? m_engine->toJsValueFromPtr(mb->addMenu(title)) : JS_EXCEPTION;
    });
    bind_menubar.def("menuAt", [this](JZObjectPointer* w, int i) -> JSValue {
        JZMenuBar* mb = widgetFromThis<JZMenuBar>(w, "menuAt");
        return mb ? m_engine->toJsValueFromPtr(mb->menuAt(i)) : JS_EXCEPTION;
    });
    bind_menubar.def("menuCount", [this](JZObjectPointer* w) {
        JZMenuBar* mb = widgetFromThis<JZMenuBar>(w, "menuCount");
        return mb ? mb->menuCount() : 0;
    });
    mountTransparentClass(bind_menubar.classDefine());

    // JZMenu —— 菜单(数据 + 弹层控制器, 非渲染树控件)。菜单项由 addAction/addMenu/addSeparator 建立。
    jsbind::ClassBind<JZMenu> bind_menu("JZMenu");
    bind_menu.def("setTitle", [](JZMenu* m, const std::string& t) { m->setTitle(t); });
    bind_menu.def("title", [](JZMenu* m) { return m->title(); });    
    bind_menu.def("addSeparator", [](JZMenu* m) { m->addSeparator(); });
    bind_menu.def("menuCount", [](JZMenu* m) { return m->menuCount(); });    
    bind_menu.def("actionCount", [](JZMenu* m) { return m->actionCount(); });    
    // 从 JS 建菜单树(loader 生成的 setupUi() / 手写右键菜单都这么用):
    //   const mb = new JZMenuBar();
    //   const m  = mb.addMenu("文件");                 // -> JZMenu
    //   const a  = m.addAction("退出");                 // -> JZAction
    //   a.onTriggered = () => { ... };  a.setShortcut("Ctrl+Q");
    //   m.popup(win, new JZRect(gx, gy, 1, 1));        // 弹层锚点 = 屏幕逻辑坐标
    // ⚠ addAction/addMenu/actionAt/menuAt 返回的都是**借用句柄**(对象由菜单/菜单栏持有, JS 不
    //   负责 delete): 句柄若被 GC, 按守卫约定会把真对象删掉 —— 所以 @jz/widget 的 menus.ts
    //   会在 JS 侧把句柄挂到宿主上保活。要"不持句柄"的写法, 见下面的下标式 setActionXxx。
    bind_menu.def("addAction", [this](JZMenu* m, const std::string& text) -> JSValue {
        return m_engine->toJsValueFromPtr(m->addAction(text));
    });
    bind_menu.def("addMenu", [this](JZMenu* m, const std::string& title) -> JSValue {
        return m_engine->toJsValueFromPtr(m->addMenu(title));
    });
    bind_menu.def("actionAt", [this](JZMenu* m, int i) -> JSValue {
        return m_engine->toJsValueFromPtr(m->actionAt(i));
    });
    bind_menu.def("menuAt", [this](JZMenu* m, int i) -> JSValue {
        return m_engine->toJsValueFromPtr(m->menuAt(i));
    });
    bind_menu.def("setActionEnabled", [](JZMenu* m, int i, bool on) {
        if (JZAction* a = m->actionAt(i)) a->setEnabled(on);
    });
    bind_menu.def("setActionChecked", [](JZMenu* m, int i, bool on) {
        if (JZAction* a = m->actionAt(i)) a->setChecked(on);
    });
    bind_menu.def("isActionChecked", [](JZMenu* m, int i) {
        JZAction* a = m->actionAt(i);
        return a ? a->isChecked() : false;
    });
    defSignalProperty(bind_menu, "onActionTriggered", &JZMenu::sigActionTriggered);
    // owner 允许 null(以前是"null 就静默不弹" —— 那样 JS 侧没有窗口句柄时右键菜单会毫无反应):
    // owner 只用于取 DPI 缩放与模态归属, 为 null 时弹层按 1.0 缩放定位(仍会正常弹出)。
    auto f_menuPopup = bind_menu.def("popup", [this](JZMenu* m, JZObjectPointer* owner, const JZRect& anchor) {
        // owner 允许 null(按 1.0 缩放定位); 传了非对象由形参层拦下(JSType_objectPointer 校验)
        m->popup(owner ? widgetFromThis<JZWindow>(owner, "popup") : nullptr, anchor);
    });
    f_menuPopup->params[1].allowNull = true;
    bind_menu.def("hidePopup", [](JZMenu* m) { m->hidePopup(); });
    bind_menu.def("isPopupOpen", [](JZMenu* m) { return m->isPopupOpen(); });
    m_engine->registClass(bind_menu.classDefine());

    // JZAction —— 菜单项(数据模型)。triggered 信号转 JS 回调。
    jsbind::ClassBind<JZAction> bind_action("JZAction");
    bind_action.def("setText", [](JZAction* a, const std::string& t) { a->setText(t); });
    bind_action.def("text", [](JZAction* a) { return a->text(); });
    bind_action.def("setShortcut", [](JZAction* a, const std::string& s) { a->setShortcut(s); });
    bind_action.def("shortcut", [](JZAction* a) { return a->shortcut(); });
    bind_action.def("setEnabled", [](JZAction* a, bool on) { a->setEnabled(on); });
    bind_action.def("isEnabled", [](JZAction* a) { return a->isEnabled(); });
    bind_action.def("setVisible", [](JZAction* a, bool on) { a->setVisible(on); });
    bind_action.def("isVisible", [](JZAction* a) { return a->isVisible(); });
    bind_action.def("setCheckable", [](JZAction* a, bool on) { a->setCheckable(on); });
    bind_action.def("isCheckable", [](JZAction* a) { return a->isCheckable(); });
    bind_action.def("setChecked", [](JZAction* a, bool on) { a->setChecked(on); });
    bind_action.def("isChecked", [](JZAction* a) { return a->isChecked(); });
    bind_action.def("setIcon", [](JZAction* a, JZImage* img) { a->setIcon(*img); });
    bind_action.def("trigger", [](JZAction* a) { a->trigger(); });
    defSignalProperty(bind_action, "onTriggered", &JZAction::triggered);
    m_engine->registClass(bind_action.classDefine());

    // JZLabel
    jsbind::ClassBind<JZLabel> bind_label("JZLabel", bind_w);
    // 宿主形参收 JSValue(引擎原样传、不做任何检测), 函数体内 dynamic_cast 到真正的类 ——
    // 传错控件类是**可捕获的 JS 异常**, 而不是"按声明类型 static_cast 出来的野指针"。
    // 为什么: 控件族对引擎透明, `JZLabel* w` 这种形参解析不到类型号(校验静默跳过), 见
    // docs/widget-binding-design.md §9。
    bind_label.def("setText", [this](JZObjectPointer* w, const std::string& text) {
        if (JZLabel* label = widgetFromThis<JZLabel>(w, "setText"))
            label->setText(text);
    });
    bind_label.def("text", [this](JZObjectPointer* w) {
        JZLabel* label = widgetFromThis<JZLabel>(w, "text");
        return label ? label->text() : std::string();
    });
    bind_label.def("setAlign", [this](JZObjectPointer* w, int a) {
        if (JZLabel* label = widgetFromThis<JZLabel>(w, "setAlign"))
            label->setAlign(a);
    });
    bind_label.setCreate(js_JZLabel_ctor);
    mountTransparentClass(bind_label.classDefine());

    // JZPushButton
    jsbind::ClassBind<JZPushButton> bind_btn("JZPushButton", bind_w);
    bind_btn.def("setText", [this](JZObjectPointer* w, const std::string& t) {
        if (JZPushButton* btn = widgetFromThis<JZPushButton>(w, "setText"))
            btn->setText(t);
    });
    bind_btn.def("text", [this](JZObjectPointer* w) {
        JZPushButton* btn = widgetFromThis<JZPushButton>(w, "text");
        return btn ? btn->text() : std::string();
    });
    // JZImage* 形参照旧交给引擎转换: JZImage 是**注册类**, 引擎的类型表认它(类型不符会拦)
    bind_btn.def("setIcon", [this](JZObjectPointer* w, JZImage* img) {
        if (JZPushButton* btn = widgetFromThis<JZPushButton>(w, "setIcon"))
            btn->setIcon(*img);
    });
    bind_btn.def("icon", [this](JZObjectPointer* w) {
        JZPushButton* btn = widgetFromThis<JZPushButton>(w, "icon");
        return btn ? btn->icon() : JZImage();
    });
    bind_btn.def("setWidth", [this](JZObjectPointer* w, int v) {
        if (JZPushButton* btn = widgetFromThis<JZPushButton>(w, "setWidth"))
            btn->setWidth(v);
    });
    bind_btn.def("setHeight", [this](JZObjectPointer* w, int v) {
        if (JZPushButton* btn = widgetFromThis<JZPushButton>(w, "setHeight"))
            btn->setHeight(v);
    });
    // 点击信号: 属性形(单发/覆盖式) —— `btn.onClicked = () => ...`; 赋 null 清空
    defSignalProperty(bind_btn, "onClicked", &JZPushButton::sigClicked);    
    bind_btn.setCreate(js_JZPushButton_ctor);
    mountTransparentClass(bind_btn.classDefine());

    // JZLineEdit (文本编辑接口在 JZAbstractTextEdit)
    // 宿主形参统一走 widgetFromThis: 成员指针 `&JZLineEdit::setText` 会把宿主推导成
    // JZAbstractTextEdit(JS 侧没有这个类), 写 lambda 才能点名要 JZLineEdit。
    jsbind::ClassBind<JZLineEdit> bind_edit("JZLineEdit", bind_w);
    bind_edit.def("setText", [this](JZObjectPointer* w, const std::string& s) {
        if (JZLineEdit* e = widgetFromThis<JZLineEdit>(w, "setText"))
            e->setText(s);
    });
    bind_edit.def("text", [this](JZObjectPointer* w) {
        JZLineEdit* e = widgetFromThis<JZLineEdit>(w, "text");
        return e ? e->text() : std::string();
    });
    bind_edit.def("setPassword", [this](JZObjectPointer* w, bool on) {
        if (JZLineEdit* e = widgetFromThis<JZLineEdit>(w, "setPassword"))
            e->setPassword(on);
    });
    bind_edit.def("isPassword", [this](JZObjectPointer* w) {
        JZLineEdit* e = widgetFromThis<JZLineEdit>(w, "isPassword");
        return e && e->isPassword();
    });
    bind_edit.def("setMaxLength", [this](JZObjectPointer* w, int n) {
        if (JZLineEdit* e = widgetFromThis<JZLineEdit>(w, "setMaxLength"))
            e->setMaxLength(n);
    });
    bind_edit.def("maxLength", [this](JZObjectPointer* w) {
        JZLineEdit* e = widgetFromThis<JZLineEdit>(w, "maxLength");
        return e ? e->maxLength() : 0;
    });
    bind_edit.def("setReadOnly", [this](JZObjectPointer* w, bool on) {
        if (JZLineEdit* e = widgetFromThis<JZLineEdit>(w, "setReadOnly"))
            e->setReadOnly(on);
    });
    bind_edit.def("isReadOnly", [this](JZObjectPointer* w) {
        JZLineEdit* e = widgetFromThis<JZLineEdit>(w, "isReadOnly");
        return e && e->isReadOnly();
    });
    bind_edit.def("setPlaceholderText", [this](JZObjectPointer* w, const std::string& s) {
        if (JZLineEdit* e = widgetFromThis<JZLineEdit>(w, "setPlaceholderText"))
            e->setPlaceholderText(s);
    });
    bind_edit.def("placeholderText", [this](JZObjectPointer* w) {
        JZLineEdit* e = widgetFromThis<JZLineEdit>(w, "placeholderText");
        return e ? e->placeholderText() : std::string();
    });
    // 文本类信号(实现在 JZAbstractTextEdit 上)
    // 文本类信号(声明在 JZAbstractTextEdit 上, 所以成员指针的宿主类型是它 —— 模板的 S 参数)
    defSignalProperty(bind_edit, "onTextChanged", &JZLineEdit::sigTextChanged);
    defSignalProperty(bind_edit, "onReturnPressed", &JZLineEdit::sigReturnPressed);
    defSignalProperty(bind_edit, "onEditingFinished", &JZLineEdit::sigEditingFinished);    
    bind_edit.setCreate(js_JZLineEdit_ctor);
    mountTransparentClass(bind_edit.classDefine());

    // JZDateEdit —— 日期编辑框(2026-10-04; C++ 侧解析/微调/日历仍是 TODO, 见 JZDateEdit.h)。
    // JS 侧这里只给"原始口径"两个方法(setDateMsecs/dateMsecs, 毫秒时间戳)：Qt 形状的
    // setDate(Date)/dateTime()/date() 由 TS 包装层补(js_wrapper/widget/dates.ts) —— 与内建 Date
    // 的换算只做一次, 而且 C++ 侧不用认识 JS 的 Date 对象。
    // 文本接口(setText/text/placeholder/文本信号)继承 JZLineEdit 那份(本类在 C++ 侧派生于它,
    // JS 侧也 extends JZLineEdit ⇒ 原型链上有; 那些方法内部 dynamic_cast<JZLineEdit*> 对本类成立)。
    jsbind::ClassBind<JZDateEdit> bind_date("JZDateEdit", bind_edit);
    bind_date.def("setDateMsecs", [this](JZObjectPointer* w, double ms) {
        if (JZDateEdit* d = widgetFromThis<JZDateEdit>(w, "setDateMsecs"))
            d->setDate(JZDateTime::fromMSecsSinceEpoch((int64_t)ms));
    });
    bind_date.def("dateMsecs", [this](JZObjectPointer* w) {
        JZDateEdit* d = widgetFromThis<JZDateEdit>(w, "dateMsecs");
        return d ? (double)d->date().toMSecsSinceEpoch() : 0.0;
    });
    bind_date.def("setDisplayFormat", [this](JZObjectPointer* w, const std::string& f) {
        if (JZDateEdit* d = widgetFromThis<JZDateEdit>(w, "setDisplayFormat"))
            d->setDisplayFormat(f);
    });
    bind_date.def("displayFormat", [this](JZObjectPointer* w) {
        JZDateEdit* d = widgetFromThis<JZDateEdit>(w, "displayFormat");
        return d ? d->displayFormat() : std::string();
    });
    bind_date.def("setCalendarPopup", [this](JZObjectPointer* w, bool on) {
        if (JZDateEdit* d = widgetFromThis<JZDateEdit>(w, "setCalendarPopup"))
            d->setCalendarPopup(on);
    });
    bind_date.def("calendarPopup", [this](JZObjectPointer* w) {
        JZDateEdit* d = widgetFromThis<JZDateEdit>(w, "calendarPopup");
        return d && d->calendarPopup();
    });
    bind_date.def("stepBy", [this](JZObjectPointer* w, int days) {
        if (JZDateEdit* d = widgetFromThis<JZDateEdit>(w, "stepBy"))
            d->stepBy(days);
    });
    defSignalProperty(bind_date, "onDateChanged", &JZDateEdit::dateChanged);   // 回调带 (Date)
    bind_date.setCreate(js_JZDateEdit_ctor);
    mountTransparentClass(bind_date.classDefine());

    // JZCheckBox
    jsbind::ClassBind<JZCheckbox> bind_check("JZCheckbox", bind_w);
    bind_check.def("setText", [this](JZObjectPointer* w, const std::string& t) {
        if (JZCheckbox* c = widgetFromThis<JZCheckbox>(w, "setText"))
            c->setText(t);
    });
    bind_check.def("text", [this](JZObjectPointer* w) {
        JZCheckbox* c = widgetFromThis<JZCheckbox>(w, "text");
        return c ? c->text() : std::string();
    });
    bind_check.def("setChecked", [this](JZObjectPointer* w, bool on) {
        if (JZCheckbox* c = widgetFromThis<JZCheckbox>(w, "setChecked"))
            c->setChecked(on);
    });
    bind_check.def("isChecked", [this](JZObjectPointer* w) {
        JZCheckbox* c = widgetFromThis<JZCheckbox>(w, "isChecked");
        return c && c->isChecked();
    });
    defSignalProperty(bind_check, "onToggled", &JZCheckbox::sigToggled);   // 回调带 (bool)    
    bind_check.setCreate(js_JZCheckbox_ctor);
    mountTransparentClass(bind_check.classDefine());

    // JZRadioButton
    jsbind::ClassBind<JZRadioButton> bind_radio("JZRadioButton", bind_w);
    bind_radio.def("setText", [this](JZObjectPointer* w, const std::string& t) {
        if (JZRadioButton* r = widgetFromThis<JZRadioButton>(w, "setText"))
            r->setText(t);
    });
    bind_radio.def("text", [this](JZObjectPointer* w) {
        JZRadioButton* r = widgetFromThis<JZRadioButton>(w, "text");
        return r ? r->text() : std::string();
    });
    bind_radio.def("setChecked", [this](JZObjectPointer* w, bool on) {
        if (JZRadioButton* r = widgetFromThis<JZRadioButton>(w, "setChecked"))
            r->setChecked(on);
    });
    bind_radio.def("isChecked", [this](JZObjectPointer* w) {
        JZRadioButton* r = widgetFromThis<JZRadioButton>(w, "isChecked");
        return r && r->isChecked();
    });
    defSignalProperty(bind_radio, "onToggled", &JZRadioButton::sigToggled);   // RadioButton 的信号**无参**    
    bind_radio.setCreate(js_JZRadioButton_ctor);
    mountTransparentClass(bind_radio.classDefine());

    // JZSlider
    jsbind::ClassBind<JZSlider> bind_slider("JZSlider", bind_w);
    bind_slider.def("setRange", [this](JZObjectPointer* w, int min, int max) {
        if (JZSlider* s = widgetFromThis<JZSlider>(w, "setRange"))
            s->setRange(min, max);
    });
    bind_slider.def("setValue", [this](JZObjectPointer* w, int v) {
        if (JZSlider* s = widgetFromThis<JZSlider>(w, "setValue"))
            s->setValue(v);
    });
    bind_slider.def("value", [this](JZObjectPointer* w) {
        JZSlider* s = widgetFromThis<JZSlider>(w, "value");
        return s ? s->value() : 0;
    });
    defSignalProperty(bind_slider, "onValueChanged", &JZSlider::sigValueChanged);   // 回调带 (int)    
    bind_slider.setCreate(js_JZSlider_ctor);
    mountTransparentClass(bind_slider.classDefine());

    // JZProgressBar
    jsbind::ClassBind<JZProgressBar> bind_bar("JZProgressBar", bind_w);
    bind_bar.def("setRange", [this](JZObjectPointer* w, int min, int max) {
        if (JZProgressBar* b = widgetFromThis<JZProgressBar>(w, "setRange"))
            b->setRange(min, max);
    });
    bind_bar.def("setValue", [this](JZObjectPointer* w, int v) {
        if (JZProgressBar* b = widgetFromThis<JZProgressBar>(w, "setValue"))
            b->setValue(v);
    });
    bind_bar.def("value", [this](JZObjectPointer* w) {
        JZProgressBar* b = widgetFromThis<JZProgressBar>(w, "value");
        return b ? b->value() : 0;
    });    
    bind_bar.setCreate(js_JZProgressBar_ctor);
    mountTransparentClass(bind_bar.classDefine());

    // JZComboBox
    jsbind::ClassBind<JZComboBox> bind_combo("JZComboBox", bind_w);
    bind_combo.def("addItem", [this](JZObjectPointer* w, const std::string& s) {
        if (JZComboBox* cb = widgetFromThis<JZComboBox>(w, "addItem"))
            cb->addItem(s);
    });
    // 形参用 JSValue 不用 vector<string>: JS 数组过不了引擎的形参类型校验(数组 class id 不在
    // 类型表里, 见 JZChart 那段注释), 所以自行转换。
    bind_combo.def("addItems", [this](JZObjectPointer* w, JSValue items) {
        if (JZComboBox* cb = widgetFromThis<JZComboBox>(w, "addItems"))
            cb->addItems(jzJsToStringArray(m_engine->context(), items, "JZComboBox.addItems"));
    });
    bind_combo.def("removeItem", [this](JZObjectPointer* w, int i) {
        if (JZComboBox* cb = widgetFromThis<JZComboBox>(w, "removeItem"))
            cb->removeItem(i);
    });
    bind_combo.def("clear", [this](JZObjectPointer* w) {
        if (JZComboBox* cb = widgetFromThis<JZComboBox>(w, "clear"))
            cb->clear();
    });
    bind_combo.def("currentIndex", [this](JZObjectPointer* w) {
        JZComboBox* cb = widgetFromThis<JZComboBox>(w, "currentIndex");
        return cb ? cb->currentIndex() : -1;
    });
    bind_combo.def("setCurrentIndex", [this](JZObjectPointer* w, int i) {
        if (JZComboBox* cb = widgetFromThis<JZComboBox>(w, "setCurrentIndex"))
            cb->setCurrentIndex(i);
    });
    bind_combo.def("currentText", [this](JZObjectPointer* w) {
        JZComboBox* cb = widgetFromThis<JZComboBox>(w, "currentText");
        return cb ? cb->currentText() : std::string();
    });
    bind_combo.def("setCurrentText", [this](JZObjectPointer* w, const std::string& s) {
        if (JZComboBox* cb = widgetFromThis<JZComboBox>(w, "setCurrentText"))
            cb->setCurrentText(s);
    });
    bind_combo.def("itemText", [this](JZObjectPointer* w, int i) {
        JZComboBox* cb = widgetFromThis<JZComboBox>(w, "itemText");
        return cb ? cb->itemText(i) : std::string();
    });
    bind_combo.def("count", [this](JZObjectPointer* w) {
        JZComboBox* cb = widgetFromThis<JZComboBox>(w, "count");
        return cb ? cb->count() : 0;
    });
    defSignalProperty(bind_combo, "onCurrentIndexChanged", &JZComboBox::currentIndexChanged);   // (int)
    defSignalProperty(bind_combo, "onCurrentTextChanged", &JZComboBox::currentTextChanged);    // (string)    
    bind_combo.setCreate(js_JZComboBox_ctor);
    mountTransparentClass(bind_combo.classDefine());

    // JZSpinBox
    jsbind::ClassBind<JZSpinBox> bind_spin("JZSpinBox", bind_w);
    bind_spin.def("setRange", [this](JZObjectPointer* w, int min, int max) {
        if (JZSpinBox* s = widgetFromThis<JZSpinBox>(w, "setRange"))
            s->setRange(min, max);
    });
    bind_spin.def("setMinimum", [this](JZObjectPointer* w, int v) {
        if (JZSpinBox* s = widgetFromThis<JZSpinBox>(w, "setMinimum"))
            s->setMinimum(v);
    });
    bind_spin.def("minimum", [this](JZObjectPointer* w) {
        JZSpinBox* s = widgetFromThis<JZSpinBox>(w, "minimum");
        return s ? s->minimum() : 0;
    });
    bind_spin.def("setMaximum", [this](JZObjectPointer* w, int v) {
        if (JZSpinBox* s = widgetFromThis<JZSpinBox>(w, "setMaximum"))
            s->setMaximum(v);
    });
    bind_spin.def("maximum", [this](JZObjectPointer* w) {
        JZSpinBox* s = widgetFromThis<JZSpinBox>(w, "maximum");
        return s ? s->maximum() : 0;
    });
    bind_spin.def("setStep", [this](JZObjectPointer* w, int v) {
        if (JZSpinBox* s = widgetFromThis<JZSpinBox>(w, "setStep"))
            s->setStep(v);
    });
    bind_spin.def("step", [this](JZObjectPointer* w) {
        JZSpinBox* s = widgetFromThis<JZSpinBox>(w, "step");
        return s ? s->step() : 0;
    });
    bind_spin.def("setValue", [this](JZObjectPointer* w, int v) {
        if (JZSpinBox* s = widgetFromThis<JZSpinBox>(w, "setValue"))
            s->setValue(v);
    });
    bind_spin.def("value", [this](JZObjectPointer* w) {
        JZSpinBox* s = widgetFromThis<JZSpinBox>(w, "value");
        return s ? s->value() : 0;
    });
    bind_spin.def("setSuffix", [this](JZObjectPointer* w, const std::string& s) {
        if (JZSpinBox* b = widgetFromThis<JZSpinBox>(w, "setSuffix"))
            b->setSuffix(s);
    });
    bind_spin.def("suffix", [this](JZObjectPointer* w) {
        JZSpinBox* s = widgetFromThis<JZSpinBox>(w, "suffix");
        return s ? s->suffix() : std::string();
    });
    bind_spin.def("setSpecialValueText", [this](JZObjectPointer* w, const std::string& s) {
        if (JZSpinBox* p = widgetFromThis<JZSpinBox>(w, "setSpecialValueText"))
            p->setSpecialValueText(s);
    });
    bind_spin.def("specialValueText", [this](JZObjectPointer* w) {
        JZSpinBox* p = widgetFromThis<JZSpinBox>(w, "specialValueText");
        return p ? p->specialValueText() : std::string();
    });
    bind_spin.def("setReadOnly", [this](JZObjectPointer* w, bool on) {
        if (JZSpinBox* s = widgetFromThis<JZSpinBox>(w, "setReadOnly"))
            s->setReadOnly(on);
    });
    bind_spin.def("isReadOnly", [this](JZObjectPointer* w) {
        JZSpinBox* s = widgetFromThis<JZSpinBox>(w, "isReadOnly");
        return s && s->isReadOnly();
    });
    defSignalProperty(bind_spin, "onValueChanged", &JZSpinBox::sigValueChanged);   // 回调带 (int)    
    bind_spin.setCreate(js_JZSpinBox_ctor);
    mountTransparentClass(bind_spin.classDefine());

    // JZDoubleSpinBox —— 小数步进框(2026-10-04): 同名方法挂在自己的原型上, 于是 setValue/minimum/
    // maximum 这些在小数框上走 double 口径(遮蔽父原型上那份); suffix/readOnly/setText 之类照样继承。
    // 注: 基类的 setStep/step 是整数口径, 小数步长用 setSingleStep/singleStep。
    jsbind::ClassBind<JZDoubleSpinBox> bind_dspin("JZDoubleSpinBox", bind_spin);
    bind_dspin.def("setRange", [this](JZObjectPointer* w, double min, double max) {
        if (JZDoubleSpinBox* s = widgetFromThis<JZDoubleSpinBox>(w, "setRange"))
            s->setRange(min, max);
    });
    bind_dspin.def("setMinimum", [this](JZObjectPointer* w, double v) {
        if (JZDoubleSpinBox* s = widgetFromThis<JZDoubleSpinBox>(w, "setMinimum"))
            s->setMinimum(v);
    });
    bind_dspin.def("minimum", [this](JZObjectPointer* w) {
        JZDoubleSpinBox* s = widgetFromThis<JZDoubleSpinBox>(w, "minimum");
        return s ? s->minimum() : 0.0;
    });
    bind_dspin.def("setMaximum", [this](JZObjectPointer* w, double v) {
        if (JZDoubleSpinBox* s = widgetFromThis<JZDoubleSpinBox>(w, "setMaximum"))
            s->setMaximum(v);
    });
    bind_dspin.def("maximum", [this](JZObjectPointer* w) {
        JZDoubleSpinBox* s = widgetFromThis<JZDoubleSpinBox>(w, "maximum");
        return s ? s->maximum() : 0.0;
    });
    bind_dspin.def("setSingleStep", [this](JZObjectPointer* w, double v) {
        if (JZDoubleSpinBox* s = widgetFromThis<JZDoubleSpinBox>(w, "setSingleStep"))
            s->setSingleStep(v);
    });
    bind_dspin.def("singleStep", [this](JZObjectPointer* w) {
        JZDoubleSpinBox* s = widgetFromThis<JZDoubleSpinBox>(w, "singleStep");
        return s ? s->singleStep() : 0.0;
    });
    bind_dspin.def("setValue", [this](JZObjectPointer* w, double v) {
        if (JZDoubleSpinBox* s = widgetFromThis<JZDoubleSpinBox>(w, "setValue"))
            s->setValue(v);
    });
    bind_dspin.def("value", [this](JZObjectPointer* w) {
        JZDoubleSpinBox* s = widgetFromThis<JZDoubleSpinBox>(w, "value");
        return s ? s->value() : 0.0;
    });
    bind_dspin.def("setDecimals", [this](JZObjectPointer* w, int n) {
        if (JZDoubleSpinBox* s = widgetFromThis<JZDoubleSpinBox>(w, "setDecimals"))
            s->setDecimals(n);
    });
    bind_dspin.def("decimals", [this](JZObjectPointer* w) {
        JZDoubleSpinBox* s = widgetFromThis<JZDoubleSpinBox>(w, "decimals");
        return s ? s->decimals() : 0;
    });
    bind_dspin.def("setPrefix", [this](JZObjectPointer* w, const std::string& s) {
        if (JZDoubleSpinBox* p = widgetFromThis<JZDoubleSpinBox>(w, "setPrefix"))
            p->setPrefix(s);
    });
    bind_dspin.def("prefix", [this](JZObjectPointer* w) {
        JZDoubleSpinBox* s = widgetFromThis<JZDoubleSpinBox>(w, "prefix");
        return s ? s->prefix() : std::string();
    });
    defSignalProperty(bind_dspin, "onValueChanged", &JZDoubleSpinBox::sigValueChanged);   // 回调带 (number)
    bind_dspin.setCreate(js_JZDoubleSpinBox_ctor);
    mountTransparentClass(bind_dspin.classDefine());

    // JZGroupBox
    jsbind::ClassBind<JZGroupBox> bind_group("JZGroupBox", bind_w);
    bind_group.def("setTitle", [this](JZObjectPointer* w, const std::string& s) {
        if (JZGroupBox* g = widgetFromThis<JZGroupBox>(w, "setTitle"))
            g->setTitle(s);
    });
    bind_group.def("title", [this](JZObjectPointer* w) {
        JZGroupBox* g = widgetFromThis<JZGroupBox>(w, "title");
        return g ? g->title() : std::string();
    });    
    bind_group.setCreate(js_JZGroupBox_ctor);
    mountTransparentClass(bind_group.classDefine());

    // JZSwitch (开关: on/off)
    jsbind::ClassBind<JZSwitch> bind_switch("JZSwitch", bind_w);
    bind_switch.def("setState", [this](JZObjectPointer* w, bool on) {
        if (JZSwitch* s = widgetFromThis<JZSwitch>(w, "setState"))
            s->setState(on);
    });
    bind_switch.def("isOn", [this](JZObjectPointer* w) {
        JZSwitch* s = widgetFromThis<JZSwitch>(w, "isOn");
        return s && s->isOn();
    });
    defSignalProperty(bind_switch, "onToggled", &JZSwitch::sigToggled);   // 回调带 (bool)    
    bind_switch.setCreate(js_JZSwitch_ctor);
    mountTransparentClass(bind_switch.classDefine());

    // JZTextEdit (多行文本编辑; 只读时可直接当滚动日志用)
    // 同 JZLineEdit: 文本接口都在 JZAbstractTextEdit(未注册) 上, 全部写 lambda 包一层。
    jsbind::ClassBind<JZTextEdit> bind_text("JZTextEdit", bind_w);
    bind_text.def("setText", [this](JZObjectPointer* w, const std::string& s) {
        if (JZTextEdit* e = widgetFromThis<JZTextEdit>(w, "setText"))
            e->setText(s);
    });
    bind_text.def("text", [this](JZObjectPointer* w) {
        JZTextEdit* e = widgetFromThis<JZTextEdit>(w, "text");
        return e ? e->text() : std::string();
    });
    bind_text.def("setPlaceholderText", [this](JZObjectPointer* w, const std::string& s) {
        if (JZTextEdit* e = widgetFromThis<JZTextEdit>(w, "setPlaceholderText"))
            e->setPlaceholderText(s);
    });
    bind_text.def("placeholderText", [this](JZObjectPointer* w) {
        JZTextEdit* e = widgetFromThis<JZTextEdit>(w, "placeholderText");
        return e ? e->placeholderText() : std::string();
    });
    bind_text.def("setReadOnly", [this](JZObjectPointer* w, bool on) {
        if (JZTextEdit* e = widgetFromThis<JZTextEdit>(w, "setReadOnly"))
            e->setReadOnly(on);
    });
    bind_text.def("isReadOnly", [this](JZObjectPointer* w) {
        JZTextEdit* e = widgetFromThis<JZTextEdit>(w, "isReadOnly");
        return e && e->isReadOnly();
    });
    bind_text.def("setLineWrap", [this](JZObjectPointer* w, bool wrap) {
        if (JZTextEdit* e = widgetFromThis<JZTextEdit>(w, "setLineWrap"))
            e->setLineWrap(wrap);
    });
    bind_text.def("lineWrap", [this](JZObjectPointer* w) {
        JZTextEdit* e = widgetFromThis<JZTextEdit>(w, "lineWrap");
        return e && e->lineWrap();
    });
    bind_text.def("cursorPosition", [this](JZObjectPointer* w) {
        JZTextEdit* e = widgetFromThis<JZTextEdit>(w, "cursorPosition");
        return e ? e->cursorPosition() : 0;
    });
    bind_text.def("setCursorPosition", [this](JZObjectPointer* w, int pos) {
        if (JZTextEdit* e = widgetFromThis<JZTextEdit>(w, "setCursorPosition"))
            e->setCursorPosition(pos);
    });
    bind_text.def("selectAll", [this](JZObjectPointer* w) {
        if (JZTextEdit* e = widgetFromThis<JZTextEdit>(w, "selectAll"))
            e->selectAll();
    });
    defSignalProperty(bind_text, "onTextChanged", &JZTextEdit::sigTextChanged);   // (string)    
    bind_text.setCreate(js_JZTextEdit_ctor);
    mountTransparentClass(bind_text.classDefine());

    // JZChart (图表: 折线 / 柱状 / 饼图)
    // 与 JZLabel 等自绘控件同样用默认构造: JZChart 的数据模型是值语义的(序列由控件内部的
    // vector 持有, 没有需要 JS 侧持有/失效的指针), 故不需要 JZJS<Native> 镜像类的那套所有权处理。
    // 唯一要额外做的是序列读取的**越界校验** —— 基类 series(i) 是 vector[i] 不做检查, JS 传错下标
    // 不能把进程带走(抛异常, 由 call() 转成 JS 异常)。
    jsbind::ClassBind<JZChart> bind_chart("JZChart", bind_w);
    bind_chart.def("setChartType", [this](JZObjectPointer* w, int type) {
        JZChart* c = widgetFromThis<JZChart>(w, "setChartType");
        if (!c)
            return;
        if (type < (int)JZChart::LineChart || type > (int)JZChart::PieChart)
            throw std::runtime_error("JZChart.setChartType: 需要 JZChart.LineChart/BarChart/PieChart");
        c->setChartType((JZChart::ChartType)type);
    });
    bind_chart.def("chartType", [this](JZObjectPointer* w) {
        JZChart* c = widgetFromThis<JZChart>(w, "chartType");
        return c ? (int)c->chartType() : 0;
    });
    // 颜色参数用 CSS 风格字符串('#rrggbb' / 'red'), 与 JZPainter.setPen/setBrush 一致;
    // 传空串取默认调色板(与 setData 的自动配色一致)。
    bind_chart.def("addSeries", [this](JZObjectPointer* w, const std::string& name, const std::string& color) {
        JZChart* c = widgetFromThis<JZChart>(w, "addSeries");
        if (!c)
            return -1;
        JZColor col = color.empty() ? JZChart::paletteColor(c->seriesCount()) : jzParseColor(color);
        return c->addSeries(name, col);
    });
    bind_chart.def("setSeriesData", [this](JZObjectPointer* w, int index, JSValue data) {
        if (JZChart* c = widgetFromThis<JZChart>(w, "setSeriesData"))
            c->setSeriesData(index, jzJsToDoubleArray(m_engine->context(), data, "JZChart.setSeriesData"));
    });
    bind_chart.def("setSeriesName", [this](JZObjectPointer* w, int index, const std::string& name) {
        if (JZChart* c = widgetFromThis<JZChart>(w, "setSeriesName"))
            c->setSeriesName(index, name);
    });
    bind_chart.def("setSeriesColor", [this](JZObjectPointer* w, int index, const std::string& color) {
        if (JZChart* c = widgetFromThis<JZChart>(w, "setSeriesColor"))
            c->setSeriesColor(index, jzParseColor(color));
    });
    bind_chart.def("clearSeries", [this](JZObjectPointer* w) {
        if (JZChart* c = widgetFromThis<JZChart>(w, "clearSeries"))
            c->clearSeries();
    });
    bind_chart.def("seriesCount", [this](JZObjectPointer* w) {
        JZChart* c = widgetFromThis<JZChart>(w, "seriesCount");
        return c ? c->seriesCount() : 0;
    });
    bind_chart.def("seriesName", [this](JZObjectPointer* w, int index) -> std::string {
        JZChart* c = widgetFromThis<JZChart>(w, "seriesName");
        if (!c)
            return std::string();
        if (index < 0 || index >= c->seriesCount())
            throw std::runtime_error("JZChart.seriesName: 序列下标越界");
        return c->series(index).name;
    });
    bind_chart.def("seriesColor", [this](JZObjectPointer* w, int index) -> JZColor {
        JZChart* c = widgetFromThis<JZChart>(w, "seriesColor");
        if (!c)
            return JZColor();
        if (index < 0 || index >= c->seriesCount())
            throw std::runtime_error("JZChart.seriesColor: 序列下标越界");
        return c->series(index).color;
    });
    bind_chart.def("seriesData", [this](JZObjectPointer* w, int index) -> std::vector<double> {
        JZChart* c = widgetFromThis<JZChart>(w, "seriesData");
        if (!c)
            return std::vector<double>();
        if (index < 0 || index >= c->seriesCount())
            throw std::runtime_error("JZChart.seriesData: 序列下标越界");
        return c->series(index).data;
    });
    // 一次性设置多条序列: setData([[1,2],[3,4]]) —— 颜色/名称用默认值(独立配色, 名称 "Series N")
    bind_chart.def("setData", [this](JZObjectPointer* w, JSValue data) {
        if (JZChart* c = widgetFromThis<JZChart>(w, "setData"))
            c->setData(jzJsToDoubleMatrix(m_engine->context(), data, "JZChart.setData"));
    });
    // 分类标签: 折线/柱状的 X 轴标签, 饼图的扇区名称
    bind_chart.def("setCategories", [this](JZObjectPointer* w, JSValue categories) {
        if (JZChart* c = widgetFromThis<JZChart>(w, "setCategories"))
            c->setCategories(jzJsToStringArray(m_engine->context(), categories, "JZChart.setCategories"));
    });
    // ---- 数值 X 轴 / 右 Y 轴 / 轴标题 (见 JZChart.h): 画"电压-容量""电流-时间"这类
    //      X 不是序号的曲线必须用它; 原来的 setCategories 只认索引对齐的字符串标签。----
    bind_chart.def("setSeriesKeys", [this](JZObjectPointer* w, int index, JSValue keys) {
        if (JZChart* c = widgetFromThis<JZChart>(w, "setSeriesKeys"))
            c->setSeriesKeys(index, jzJsToDoubleArray(m_engine->context(), keys, "JZChart.setSeriesKeys"));
    });
    bind_chart.def("setSeriesDataXY", [this](JZObjectPointer* w, int index, JSValue keys, JSValue values) {
        if (JZChart* c = widgetFromThis<JZChart>(w, "setSeriesDataXY"))
            c->setSeriesDataXY(index,
                jzJsToDoubleArray(m_engine->context(), keys, "JZChart.setSeriesDataXY"),
                jzJsToDoubleArray(m_engine->context(), values, "JZChart.setSeriesDataXY"));
    });
    bind_chart.def("seriesKeys", [this](JZObjectPointer* w, int index) -> std::vector<double> {
        JZChart* c = widgetFromThis<JZChart>(w, "seriesKeys");
        if (!c)
            return std::vector<double>();
        if (index < 0 || index >= c->seriesCount())
            throw std::runtime_error("JZChart.seriesKeys: 序列下标越界");
        return c->seriesKeys(index);
    });
    bind_chart.def("hasSeriesKeys", [this](JZObjectPointer* w, int index) {
        JZChart* c = widgetFromThis<JZChart>(w, "hasSeriesKeys");
        return c && c->hasSeriesKeys(index);
    });
    bind_chart.def("setSeriesValueAxis", [this](JZObjectPointer* w, int index, int axis) {
        JZChart* c = widgetFromThis<JZChart>(w, "setSeriesValueAxis");
        if (!c)
            return;
        if (axis != 0 && axis != 1)
            throw std::runtime_error("JZChart.setSeriesValueAxis: 轴只能是 0(左)/1(右)");
        c->setSeriesValueAxis(index, axis);
    });
    bind_chart.def("seriesValueAxis", [this](JZObjectPointer* w, int index) {
        JZChart* c = widgetFromThis<JZChart>(w, "seriesValueAxis");
        return c ? c->seriesValueAxis(index) : 0;
    });
    bind_chart.def("hasValueAxis", [this](JZObjectPointer* w, int axis) {
        JZChart* c = widgetFromThis<JZChart>(w, "hasValueAxis");
        return c && c->hasValueAxis(axis);
    });
    // 轴标题: 三个都要给(不用的轴传空串) —— 引擎按精确参数个数校验, 不做可选参数
    bind_chart.def("setAxisLabels", [this](JZObjectPointer* w, const std::string& xl,
                                           const std::string& yl, const std::string& y2l) {
        if (JZChart* c = widgetFromThis<JZChart>(w, "setAxisLabels"))
            c->setAxisLabels(xl, yl, y2l);
    });
    bind_chart.def("xAxisLabel", [this](JZObjectPointer* w) {
        JZChart* c = widgetFromThis<JZChart>(w, "xAxisLabel");
        return c ? c->xAxisLabel() : std::string();
    });
    bind_chart.def("yAxisLabel", [this](JZObjectPointer* w) {
        JZChart* c = widgetFromThis<JZChart>(w, "yAxisLabel");
        return c ? c->yAxisLabel() : std::string();
    });
    bind_chart.def("y2AxisLabel", [this](JZObjectPointer* w) {
        JZChart* c = widgetFromThis<JZChart>(w, "y2AxisLabel");
        return c ? c->y2AxisLabel() : std::string();
    });
    bind_chart.def("categories", [this](JZObjectPointer* w) {
        JZChart* c = widgetFromThis<JZChart>(w, "categories");
        // 值语义: 返回拷贝(C++ 侧返回的是 const 引用, 这里 decay 成值再拷一份)
        return c ? c->categories() : std::decay_t<decltype(c->categories())>();
    });
    bind_chart.def("setTitle", [this](JZObjectPointer* w, const std::string& title) {
        if (JZChart* c = widgetFromThis<JZChart>(w, "setTitle"))
            c->setTitle(title);
    });
    bind_chart.def("title", [this](JZObjectPointer* w) {
        JZChart* c = widgetFromThis<JZChart>(w, "title");
        return c ? c->title() : std::string();
    });
    bind_chart.def("setShowGrid", [this](JZObjectPointer* w, bool on) {
        if (JZChart* c = widgetFromThis<JZChart>(w, "setShowGrid"))
            c->setShowGrid(on);
    });
    bind_chart.def("setShowLegend", [this](JZObjectPointer* w, bool on) {
        if (JZChart* c = widgetFromThis<JZChart>(w, "setShowLegend"))
            c->setShowLegend(on);
    });
    bind_chart.def("setShowAxis", [this](JZObjectPointer* w, bool on) {
        if (JZChart* c = widgetFromThis<JZChart>(w, "setShowAxis"))
            c->setShowAxis(on);
    });
    bind_chart.def("setShowPoint", [this](JZObjectPointer* w, bool on) {
        if (JZChart* c = widgetFromThis<JZChart>(w, "setShowPoint"))
            c->setShowPoint(on);
    });
    bind_chart.def("setShowValue", [this](JZObjectPointer* w, bool on) {
        if (JZChart* c = widgetFromThis<JZChart>(w, "setShowValue"))
            c->setShowValue(on);
    });    
    bind_chart.setCreate(js_JZChart_ctor);
    mountTransparentClass(bind_chart.classDefine());

    // 类型常量挂到构造器上, JS 侧写 JZChart.BarChart(与 C++ 枚举同值)。
    // 注: 控件族对引擎透明, 所以这里直接读**全局类对象**(TS 类层定义的 JZChart)。
    //     JS_SetPropertyStr 会消费 value, 取出来的 js_class 是新引用, 用完 free。
    {
        JSContext* ctx = m_engine->context();
        JSValue js_class = JS_GetPropertyStr(ctx, m_engine->globalObject(), "JZChart");
        if (JS_IsFunction(ctx, js_class))
        {
            JS_SetPropertyStr(ctx, js_class, "LineChart", JS_NewInt32(ctx, (int)JZChart::LineChart));
            JS_SetPropertyStr(ctx, js_class, "BarChart", JS_NewInt32(ctx, (int)JZChart::BarChart));
            JS_SetPropertyStr(ctx, js_class, "PieChart", JS_NewInt32(ctx, (int)JZChart::PieChart));
        }
        JS_FreeValue(ctx, js_class);
    }

}

// ---------- 列表 / 表格: 不暴露 item 对象 ----------
// 结论(2026-09-18 定稿, 2026-09-21 复核): **不给 JS 任何 `JZTableWidgetItem` /
// `JZListWidgetItem` 句柄** —— 那套"JS 拥有 / 所有权翻转 / 借用句柄失效"的语义对调用方太贵
// (而且当初从未接线, 属死代码)。JS 侧只有**索引式值接口**(与 C++ 同名同义, 一一对应):
//   * 行/项粒度: setRows/rows/appendRow/insertRow/removeRow/insertRowAt/removeRowAt/setRowAt,
//     列表 setItems/items/appendItem/insertItem/removeItem;
//   * 单元格/单项: setItemText/itemText(+ 列表 itemData/setItemData 业务数据槽);
//   * 结构/表现: setRowCount/insertColumn/removeColumn/setHeaderLabels/headerLabels/
//     setRowHeight/setColumnWidth/clear。
// 需要"数组式"批量操作时, JS 侧显式取快照再回写(rows() -> 改 -> setRows()), 不做镜像层。
// 只读"外部改过数据"用 onDataChanged(sigDataChanged): 它只是通知, 不携带数据、不参与回灌。

// ---------- 容器 / 视图类控件 ----------
//
// 这些控件的数据/子项由 C++ 持有, JS 侧只做"索引式值操作"(文本、行数、当前项、滚动位置),
// **不向 JS 暴露 item 句柄**(见上面那段结论)。
void JZJSWidgetModule::registViews()
{
    // 基类绑定: 只为拿 superCtypeid(JZWidget 已在 registWidgets 挂好; 透明类不必注册到引擎)
    jsbind::ClassBind<JZWidget> bind_w("JZWidget");

    // ---- JZListWidget: JS 侧只做值级操作(文本/行数/当前项), item 对象不从 JS 创建 ----
    jsbind::ClassBind<JZListWidget> bind_list("JZListWidget", bind_w);
    bind_list.def("appendItem", [this](JZObjectPointer* w, const std::string& t) {
        if (JZListWidget* l = widgetFromThis<JZListWidget>(w, "appendItem"))
            l->appendItem(t);
    });
    bind_list.def("appendItems", [this](JZObjectPointer* w, JSValue texts) {
        if (JZListWidget* l = widgetFromThis<JZListWidget>(w, "appendItems"))
            l->appendItems(jzJsToStringArray(m_engine->context(), texts, "JZListWidget.appendItems"));
    });
    bind_list.def("insertItem", [this](JZObjectPointer* w, int i, const std::string& t) {
        if (JZListWidget* l = widgetFromThis<JZListWidget>(w, "insertItem"))
        {
            l->insertItem(i, t);
            syncItemData(l, 0, i);   // 业务数据数组跟着后移(新项无数据)
        }
    });
    bind_list.def("clear", [this](JZObjectPointer* w) {
        if (JZListWidget* l = widgetFromThis<JZListWidget>(w, "clear"))
        {
            l->clear();
            syncItemData(l, 2, 0);
        }
    });
    bind_list.def("count", [this](JZObjectPointer* w) {
        JZListWidget* l = widgetFromThis<JZListWidget>(w, "count");
        return l ? l->count() : 0;
    });
    bind_list.def("itemText", [this](JZObjectPointer* w, int i) {
        JZListWidget* l = widgetFromThis<JZListWidget>(w, "itemText");
        return l ? l->itemText(i) : std::string();
    });
    bind_list.def("currentIndex", [this](JZObjectPointer* w) {
        JZListWidget* l = widgetFromThis<JZListWidget>(w, "currentIndex");
        return l ? l->currentIndex() : -1;
    });
    bind_list.def("setCurrentIndex", [this](JZObjectPointer* w, int i) {
        if (JZListWidget* l = widgetFromThis<JZListWidget>(w, "setCurrentIndex"))
            l->setCurrentIndex(i);
    });
    bind_list.def("setItemHeight", [this](JZObjectPointer* w, int h) {
        if (JZListWidget* l = widgetFromThis<JZListWidget>(w, "setItemHeight"))
            l->setItemHeight(h);
    });
    bind_list.def("itemHeight", [this](JZObjectPointer* w) {
        JZListWidget* l = widgetFromThis<JZListWidget>(w, "itemHeight");
        return l ? l->itemHeight() : 0;
    });
    defSignalProperty(bind_list, "onCurrentIndexChanged", &JZListWidget::currentIndexChanged);
    defSignalProperty(bind_list, "onActivated", &JZListWidget::sigActivated);
    // ---- 整项/整块原语(索引式值接口; 不引入任何 item 句柄) ----
    // 与 C++ 的 setItems/items/removeItem/setItemText 一一对应; "整块喂 / 整块读 / 单项增删"。
    bind_list.def("setItems", [this](JZObjectPointer* w, JSValue texts) {
        if (JZListWidget* l = widgetFromThis<JZListWidget>(w, "setItems"))
        {
            l->clear();
            l->appendItems(jzJsToStringArray(m_engine->context(), texts, "JZListWidget.setItems"));
            syncItemData(l, 2, 0);   // 整块重建: 业务数据也重来
        }
    });
    bind_list.def("items", [this](JZObjectPointer* w) {
        JZListWidget* l = widgetFromThis<JZListWidget>(w, "items");
        std::vector<std::string> v;
        if (!l)
            return v;
        v.reserve(l->count());
        for (int i = 0; i < l->count(); i++)
            v.push_back(l->itemText(i));
        return v;
    });
    bind_list.def("removeItem", [this](JZObjectPointer* w, int i) {
        if (JZListWidget* l = widgetFromThis<JZListWidget>(w, "removeItem"))
        {
            delete l->takeItem(i);   // 摘除并真删
            syncItemData(l, 1, i);   // 业务数据数组同步摘掉
        }
    });
    bind_list.def("setItemText", [this](JZObjectPointer* w, int i, const std::string& s) {
        JZListWidget* l = widgetFromThis<JZListWidget>(w, "setItemText");
        if (JZListWidgetItem* it = l ? l->item(i) : nullptr)
            it->setText(s);
    });
    // 项的业务数据槽: 原样持有 JS 值(取回来还是同一个对象), 与文本分开 —— 老代码用 item.setData(...)
    // 挂 [netId, DeviceString] 这类负载, 迁到值级接口后用这两个口子(不回到 item 句柄)。
    //
    // ⚠ 数据存在 **JS 侧**: 挂在列表自己的 JS 包装对象的动态属性上(见 itemDataArray)。
    //   为什么不在 C++ 侧存: 以前是存进 JZListWidgetItem 的 JZVariant(包 JZJSValuePtr), 而列表归
    //   JS 所有、要等 JS_FreeRuntime 里那次 GC 才析构 —— 那时 ctx 已释放, guard 的 JS_FreeValue
    //   放不动, 引用计数停在 1 -> gc_obj_list 非空 -> quickjs.c:2350 断言(2026-10-04 踩过)。
    //   数据放 JS 堆里则随列表的 JS 对象一起被 GC, 生命周期天然正确, 也省掉 JZVariant 往返。
    //   (项增删时数组要同步, 见 syncItemData —— 保持"槽跟着项走"的语义。)
    bind_list.def("setItemData", [this](JZObjectPointer* w, int i, JSValue v) {
        JZListWidget* l = widgetFromThis<JZListWidget>(w, "setItemData");
        if (!l)
            return;
        if (!l->item(i))
            throw std::runtime_error("JZListWidget.setItemData: 下标越界");

        JSContext* ctx = m_engine->context();
        JSValue arr = itemDataArray(l, true);
        if (JS_IsUndefined(arr))
            return;   // 拿不到 JS 包装(理论上不会: 本调用就是从它的 JS 对象发出的)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_DupValue(ctx, v));   // 属性持一份
        JS_FreeValue(ctx, arr);
    });
    bind_list.def("itemData", [this](JZObjectPointer* w, int i) -> JSValue {
        JZListWidget* l = widgetFromThis<JZListWidget>(w, "itemData");
        if (!l)
            return JS_EXCEPTION;

        JZListWidgetItem* it = l->item(i);
        if (!it)
            return JS_UNDEFINED;

        JSContext* ctx = m_engine->context();

        // JS 侧那份(交回同一个对象, 不走任何转换)
        JSValue arr = itemDataArray(l, false);
        if (!JS_IsUndefined(arr))
        {
            JSValue v = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
            JS_FreeValue(ctx, arr);
            if (!JS_IsUndefined(v))
                return v;   // 新引用, 交给 JS
            JS_FreeValue(ctx, v);
        }

        // 回退: C++ 侧直接塞过的原生值(字符串/整数/浮点/布尔)顺手转一下, 其余给 undefined。
        const JZVariant& d = it->data();
        const int t = d.typeId();
        if (t == jzMetaTypeId<bool>())
            return JS_NewBool(ctx, d.toBool());
        if (t == jzMetaTypeId<int>())
            return JS_NewInt32(ctx, d.toInt());
        if (t == jzMetaTypeId<int64_t>())
            return JS_NewInt64(ctx, d.toInt64());
        if (t == jzMetaTypeId<double>())
            return JS_NewFloat64(ctx, d.toDouble());
        if (t == jzMetaTypeId<std::string>())
            return JS_NewString(ctx, d.toString().c_str());
        return JS_UNDEFINED;
    });
    defSignalProperty(bind_list, "onDataChanged", &JZListWidget::sigDataChanged);

    bind_list.setCreate(js_JZListWidget_ctor);
    mountTransparentClass(bind_list.classDefine());

    // ---- JZTableWidget: 同上(item 对象不走 JS) ----
    jsbind::ClassBind<JZTableWidget> bind_table("JZTableWidget", bind_w);
    bind_table.def("setRowCount", [this](JZObjectPointer* w, int n) {
        if (JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "setRowCount"))
            t->setRowCount(n);
    });
    bind_table.def("rowCount", [this](JZObjectPointer* w) {
        JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "rowCount");
        return t ? t->rowCount() : 0;
    });
    bind_table.def("setColumnCount", [this](JZObjectPointer* w, int n) {
        if (JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "setColumnCount"))
            t->setColumnCount(n);
    });
    bind_table.def("columnCount", [this](JZObjectPointer* w) {
        JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "columnCount");
        return t ? t->columnCount() : 0;
    });
    bind_table.def("setItemText", [this](JZObjectPointer* w, int r, int c, const std::string& s) {
        if (JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "setItemText"))
            t->setItemText(r, c, s);
    });
    bind_table.def("itemText", [this](JZObjectPointer* w, int r, int c) {
        JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "itemText");
        return t ? t->itemText(r, c) : std::string();
    });
    bind_table.def("clearContents", [this](JZObjectPointer* w) {
        if (JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "clearContents"))
            t->clearContents();
    });
    bind_table.def("setCurrentCell", [this](JZObjectPointer* w, int r, int c) {
        if (JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "setCurrentCell"))
            t->setCurrentCell(r, c);
    });
    bind_table.def("currentRow", [this](JZObjectPointer* w) {
        JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "currentRow");
        return t ? t->currentRow() : -1;
    });
    bind_table.def("currentColumn", [this](JZObjectPointer* w) {
        JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "currentColumn");
        return t ? t->currentColumn() : -1;
    });
    // 选中粒度: 0 = 单元格(默认, Qt SelectItems), 1 = 整行(Qt SelectRows) —— 取值同 C++ 枚举
    bind_table.def("setSelectionBehavior", [this](JZObjectPointer* w, int b) {
        if (JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "setSelectionBehavior"))
            t->setSelectionBehavior((JZTableWidget::SelectionBehavior)b);
    });
    bind_table.def("selectionBehavior", [this](JZObjectPointer* w) {
        JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "selectionBehavior");
        return t ? (int)t->selectionBehavior() : 0;
    });
    defSignalProperty(bind_table, "onCurrentRowChanged", &JZTableWidget::currentRowChanged);
    defSignalProperty(bind_table, "onCurrentColumnChanged", &JZTableWidget::currentColumnChanged);

    // ---- 整行原语(索引式值接口; 见 JZListWidget 那段说明) ----
    // "整行"= 字符串数组; 行比列宽时会自动加列(喂数据不用先算列数), 空串 = 清该格。
    bind_table.def("setRows", [this](JZObjectPointer* w, JSValue rows) {
        JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "setRows");
        if (!t)
            return;
        const std::vector<std::vector<std::string>> data =
            jzJsToRows(m_engine->context(), rows, "JZTableWidget.setRows");
        size_t maxCols = 0;
        for (const auto& r : data)
            maxCols = std::max(maxCols, r.size());

        t->setColumnCount((int)maxCols);
        t->setRowCount((int)data.size());
        for (int r = 0; r < (int)data.size(); r++)
            for (int c = 0; c < (int)data[r].size(); c++)
                t->setItemText(r, c, data[r][c]);
    });
    bind_table.def("rows", [this](JZObjectPointer* w) {
        JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "rows");
        if (!t)
            return JS_EXCEPTION;
        std::vector<std::vector<std::string>> data;
        data.reserve(t->rowCount());
        for (int r = 0; r < t->rowCount(); r++)
        {
            std::vector<std::string> row;
            row.reserve(t->columnCount());
            for (int c = 0; c < t->columnCount(); c++)
                row.push_back(t->itemText(r, c));
            data.push_back(row);
        }
        return jzRowsToJs(m_engine->context(), data);
    });
    bind_table.def("appendRow", [this](JZObjectPointer* w, JSValue cells) {
        JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "appendRow");
        if (!t)
            return;
        const std::vector<std::string> row =
            jzJsToRow(m_engine->context(), cells, "JZTableWidget.appendRow");
        if ((int)row.size() > t->columnCount())
            t->setColumnCount((int)row.size());

        int r = t->rowCount();
        t->insertRow(r);
        for (int c = 0; c < (int)row.size(); c++)
            t->setItemText(r, c, row[c]);
    });
    bind_table.def("insertRowAt", [this](JZObjectPointer* w, int i, JSValue cells) {
        JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "insertRowAt");
        if (!t)
            return;
        const std::vector<std::string> row =
            jzJsToRow(m_engine->context(), cells, "JZTableWidget.insertRowAt");
        if (i < 0 || i > t->rowCount())
            throw std::runtime_error("JZTableWidget.insertRowAt: 行号越界");
        if ((int)row.size() > t->columnCount())
            t->setColumnCount((int)row.size());

        t->insertRow(i);
        for (int c = 0; c < (int)row.size(); c++)
            t->setItemText(i, c, row[c]);
    });
    bind_table.def("removeRowAt", [this](JZObjectPointer* w, int i) {
        if (JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "removeRowAt"))
            t->removeRow(i);
    });
    bind_table.def("setRowAt", [this](JZObjectPointer* w, int i, JSValue cells) {
        JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "setRowAt");
        if (!t)
            return;
        if (i < 0 || i >= t->rowCount())
            throw std::runtime_error("JZTableWidget.setRowAt: 行号越界");
        const std::vector<std::string> row =
            jzJsToRow(m_engine->context(), cells, "JZTableWidget.setRowAt");
        if ((int)row.size() > t->columnCount())
            t->setColumnCount((int)row.size());

        for (int c = 0; c < t->columnCount(); c++)   // 整行替换: 多出来的格子清空
            t->setItemText(i, c, c < (int)row.size() ? row[c] : std::string());
    });
    bind_table.def("setHeaderLabels", [this](JZObjectPointer* w, JSValue labels) {
        if (JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "setHeaderLabels"))
            t->setHorizontalHeaderLabels(jzJsToStringArray(m_engine->context(), labels, "JZTableWidget.setHeaderLabels"));
    });
    bind_table.def("headerLabels", [this](JZObjectPointer* w) {
        JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "headerLabels");
        return t ? t->headerLabels() : std::decay_t<decltype(t->headerLabels())>();
    });
    // 行列结构(索引式): 插/删空行空列; clear() = 清数据 + 清表头(clearContents 只清数据)
    bind_table.def("insertRow", [this](JZObjectPointer* w, int i) {
        if (JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "insertRow"))
            t->insertRow(i);
    });
    bind_table.def("removeRow", [this](JZObjectPointer* w, int i) {
        if (JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "removeRow"))
            t->removeRow(i);
    });
    bind_table.def("insertColumn", [this](JZObjectPointer* w, int c) {
        if (JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "insertColumn"))
            t->insertColumn(c);
    });
    bind_table.def("removeColumn", [this](JZObjectPointer* w, int c) {
        if (JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "removeColumn"))
            t->removeColumn(c);
    });
    bind_table.def("clear", [this](JZObjectPointer* w) {
        if (JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "clear"))
            t->clear();
    });
    // 行几何 / 列宽(把"点击位置"换算成行号要用: 表格没有 itemAt/单元格控件, 鼠标事件在 JS 侧处理)
    bind_table.def("rowHeight", [this](JZObjectPointer* w) {
        JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "rowHeight");
        return t ? t->rowHeight() : 0;
    });
    bind_table.def("headerHeight", [this](JZObjectPointer* w) {
        JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "headerHeight");
        return t ? t->headerHeight() : 0;
    });
    bind_table.def("setRowHeight", [this](JZObjectPointer* w, int h) {
        if (JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "setRowHeight"))
            t->setRowHeight(h);
    });
    bind_table.def("setColumnWidth", [this](JZObjectPointer* w, int c, int wd) {
        if (JZTableWidget* t = widgetFromThis<JZTableWidget>(w, "setColumnWidth"))
            t->setColumnWidth(c, wd);
    });
    defSignalProperty(bind_table, "onDataChanged", &JZTableWidget::sigDataChanged);

    bind_table.setCreate(js_JZTableWidget_ctor);
    mountTransparentClass(bind_table.classDefine());

    // ---- JZStackedWidget: 页容器(对象形参 = JZObjectPointer*: 形参层先校验, 方法体再 dynamic_cast) ----
    jsbind::ClassBind<JZStackedWidget> bind_stack("JZStackedWidget", bind_w);
    bind_stack.def("addWidget", [this](JZObjectPointer* s, JZObjectPointer* w) -> int {
        JZStackedWidget* stack = widgetFromThis<JZStackedWidget>(s, "addWidget");
        JZWidget* page = widgetFromThis<JZWidget>(w, "addWidget");   // 传 null 已由形参层报错
        if (!stack || !page)
            return -1;
        return stack->addWidget(page);
    });
    bind_stack.def("insertWidget", [this](JZObjectPointer* s, int i, JZObjectPointer* w) {
        JZStackedWidget* stack = widgetFromThis<JZStackedWidget>(s, "insertWidget");
        JZWidget* page = widgetFromThis<JZWidget>(w, "insertWidget");
        if (!stack || !page)
            return;
        stack->insertWidget(i, page);
    });
    bind_stack.def("removeWidget", [this](JZObjectPointer* s, int i) {
        if (JZStackedWidget* stack = widgetFromThis<JZStackedWidget>(s, "removeWidget"))
            stack->removeWidget(i);
    });
    bind_stack.def("count", [this](JZObjectPointer* s) {
        JZStackedWidget* stack = widgetFromThis<JZStackedWidget>(s, "count");
        return stack ? stack->count() : 0;
    });
    bind_stack.def("indexOf", [this](JZObjectPointer* s, JZObjectPointer* w) {
        JZStackedWidget* stack = widgetFromThis<JZStackedWidget>(s, "indexOf");
        if (!stack)
            return -1;
        JZWidget* page = widgetFromThis<JZWidget>(w, "indexOf");
        return page ? stack->indexOf(page) : -1;
    });
    bind_stack.def("currentIndex", [this](JZObjectPointer* s) {
        JZStackedWidget* stack = widgetFromThis<JZStackedWidget>(s, "currentIndex");
        return stack ? stack->currentIndex() : -1;
    });
    bind_stack.def("setCurrentIndex", [this](JZObjectPointer* s, int i) {
        if (JZStackedWidget* stack = widgetFromThis<JZStackedWidget>(s, "setCurrentIndex"))
            stack->setCurrentIndex(i);
    });
    // 回吐页控件(返回**同一份 JS 对象**, `===` 成立; 见 widgetJsView):
    // 原来 C++ 侧不绑这几条, 靠 JS 便利层(tree.ts)的登记表回答 —— 但那张表只认"经 JS 入口
    // 建立的关系", 页面若是 C++ 侧挂上去的(或关系经别的路径建立)就答不出来。改为引擎侧
    // 优先还控件的宿主句柄, 覆盖面更大; 从没被 JS 拿到过的页返回 null(那本来也没有对象可回吐)。
    bind_stack.def("widget", [this](JZObjectPointer* s, int i) -> JSValue {
        JZStackedWidget* stack = widgetFromThis<JZStackedWidget>(s, "widget");
        return stack ? widgetJsView(stack->widget(i)) : JS_EXCEPTION;
    });
    bind_stack.def("currentWidget", [this](JZObjectPointer* s) -> JSValue {
        JZStackedWidget* stack = widgetFromThis<JZStackedWidget>(s, "currentWidget");
        return stack ? widgetJsView(stack->currentWidget()) : JS_EXCEPTION;
    });
    // 传 null 也能过形参校验(等价 Qt "给个空指针就什么都不做")
    {
        auto* f = bind_stack.def("setCurrentWidget", [this](JZObjectPointer* s, JZObjectPointer* w) {
            JZStackedWidget* stack = widgetFromThis<JZStackedWidget>(s, "setCurrentWidget");
            if (!stack)
                return;
            // 非本容器的页 / null -> 静默忽略(Qt 同款); 传非对象由形参层拦下
            stack->setCurrentWidget(w ? widgetFromThis<JZWidget>(w, "setCurrentWidget") : nullptr);
        });
        f->params[1].allowNull = true;   // params[0] 是 this; 页 = null ⇒ 静默忽略
    }
    bind_stack.setCreate(js_JZStackedWidget_ctor);
    mountTransparentClass(bind_stack.classDefine());

    // ---- JZTabWidget: 页签容器 ----
    jsbind::ClassBind<JZTabWidget> bind_tab("JZTabWidget", bind_w);
    bind_tab.def("addTab", [this](JZObjectPointer* t, JZObjectPointer* page, const std::string& label) -> int {
        JZTabWidget* tab = widgetFromThis<JZTabWidget>(t, "addTab");
        JZWidget* p = widgetFromThis<JZWidget>(page, "addTab");   // 传 null 已由形参层报错
        if (!tab || !p)
            return -1;
        return tab->addTab(p, label);
    });
    bind_tab.def("removeTab", [this](JZObjectPointer* t, int i) {
        if (JZTabWidget* tab = widgetFromThis<JZTabWidget>(t, "removeTab"))
            tab->removeTab(i);
    });
    bind_tab.def("count", [this](JZObjectPointer* t) {
        JZTabWidget* tab = widgetFromThis<JZTabWidget>(t, "count");
        return tab ? tab->count() : 0;
    });
    bind_tab.def("indexOf", [this](JZObjectPointer* t, JZObjectPointer* page) {
        JZTabWidget* tab = widgetFromThis<JZTabWidget>(t, "indexOf");
        if (!tab)
            return -1;
        JZWidget* p = widgetFromThis<JZWidget>(page, "indexOf");
        return p ? tab->indexOf(p) : -1;
    });
    bind_tab.def("currentIndex", [this](JZObjectPointer* t) {
        JZTabWidget* tab = widgetFromThis<JZTabWidget>(t, "currentIndex");
        return tab ? tab->currentIndex() : -1;
    });
    bind_tab.def("setCurrentIndex", [this](JZObjectPointer* t, int i) {
        if (JZTabWidget* tab = widgetFromThis<JZTabWidget>(t, "setCurrentIndex"))
            tab->setCurrentIndex(i);
    });
    bind_tab.def("tabText", [this](JZObjectPointer* t, int i) {
        JZTabWidget* tab = widgetFromThis<JZTabWidget>(t, "tabText");
        return tab ? tab->tabText(i) : std::string();
    });
    // 同 JZStackedWidget: 回吐页控件(见 widgetJsView)
    bind_tab.def("widget", [this](JZObjectPointer* t, int i) -> JSValue {
        JZTabWidget* tab = widgetFromThis<JZTabWidget>(t, "widget");
        return tab ? widgetJsView(tab->widget(i)) : JS_EXCEPTION;
    });
    bind_tab.def("currentWidget", [this](JZObjectPointer* t) -> JSValue {
        JZTabWidget* tab = widgetFromThis<JZTabWidget>(t, "currentWidget");
        return tab ? widgetJsView(tab->currentWidget()) : JS_EXCEPTION;
    });
    {
        auto* f = bind_tab.def("setCurrentWidget", [this](JZObjectPointer* t, JZObjectPointer* page) {
            JZTabWidget* tab = widgetFromThis<JZTabWidget>(t, "setCurrentWidget");
            if (!tab)
                return;
            tab->setCurrentWidget(page ? widgetFromThis<JZWidget>(page, "setCurrentWidget") : nullptr);
        });
        f->params[1].allowNull = true;   // params[0] 是 this; 页 = null ⇒ 静默忽略
    }
    defSignalProperty(bind_tab, "onCurrentChanged", &JZTabWidget::currentChanged);
    bind_tab.setCreate(js_JZTabWidget_ctor);
    mountTransparentClass(bind_tab.classDefine());

    // ---- JZSplitter: 可拖动分隔 ----
    jsbind::ClassBind<JZSplitter> bind_split("JZSplitter", bind_w);
    bind_split.def("addWidget", [this](JZObjectPointer* s, JZObjectPointer* w) {
        JZSplitter* split = widgetFromThis<JZSplitter>(s, "addWidget");
        JZWidget* child = widgetFromThis<JZWidget>(w, "addWidget");   // 传 null 已由形参层报错
        if (!split || !child)
            return;
        split->addWidget(child);
    });
    bind_split.def("insertWidget", [this](JZObjectPointer* s, int i, JZObjectPointer* w) {
        JZSplitter* split = widgetFromThis<JZSplitter>(s, "insertWidget");
        JZWidget* child = widgetFromThis<JZWidget>(w, "insertWidget");
        if (!split || !child)
            return;
        split->insertWidget(i, child);
    });
    bind_split.def("removeWidget", [this](JZObjectPointer* s, JZObjectPointer* w) {
        JZSplitter* split = widgetFromThis<JZSplitter>(s, "removeWidget");
        if (!split)
            return;
        if (JZWidget* child = widgetFromThis<JZWidget>(w, "removeWidget"))
            split->removeWidget(child);
    });
    bind_split.def("count", [this](JZObjectPointer* s) {
        JZSplitter* split = widgetFromThis<JZSplitter>(s, "count");
        return split ? split->count() : 0;
    });
    bind_split.def("indexOf", [this](JZObjectPointer* s, JZObjectPointer* w) {
        JZSplitter* split = widgetFromThis<JZSplitter>(s, "indexOf");
        if (!split)
            return -1;
        JZWidget* child = widgetFromThis<JZWidget>(w, "indexOf");
        return child ? split->indexOf(child) : -1;
    });
    // 回吐子控件(见 widgetJsView); 拿不到 JS 包装的(从没被 JS 拿到过)返回 null
    bind_split.def("widget", [this](JZObjectPointer* s, int i) -> JSValue {
        JZSplitter* split = widgetFromThis<JZSplitter>(s, "widget");
        return split ? widgetJsView(split->widget(i)) : JS_EXCEPTION;
    });
    // 方向用整数(与 JS 侧 types 里的枚举值一致): JZSplitter::Orientation 是本类内的嵌套枚举
    bind_split.def("setOrientation", [this](JZObjectPointer* s, int o) {
        if (JZSplitter* split = widgetFromThis<JZSplitter>(s, "setOrientation"))
            split->setOrientation((JZSplitter::Orientation)o);
    });
    bind_split.def("orientation", [this](JZObjectPointer* s) {
        JZSplitter* split = widgetFromThis<JZSplitter>(s, "orientation");
        return split ? (int)split->orientation() : 0;
    });
    bind_split.def("setStretchFactor", [this](JZObjectPointer* s, int i, int v) {
        if (JZSplitter* split = widgetFromThis<JZSplitter>(s, "setStretchFactor"))
            split->setStretchFactor(i, v);
    });
    bind_split.def("stretchFactor", [this](JZObjectPointer* s, int i) {
        JZSplitter* split = widgetFromThis<JZSplitter>(s, "stretchFactor");
        return split ? split->stretchFactor(i) : 0;
    });
    bind_split.def("setHandleWidth", [this](JZObjectPointer* s, int w) {
        if (JZSplitter* split = widgetFromThis<JZSplitter>(s, "setHandleWidth"))
            split->setHandleWidth(w);
    });
    bind_split.def("handleWidth", [this](JZObjectPointer* s) {
        JZSplitter* split = widgetFromThis<JZSplitter>(s, "handleWidth");
        return split ? split->handleWidth() : 0;
    });
    bind_split.def("setCollapsible", [this](JZObjectPointer* s, int i, bool c) {
        if (JZSplitter* split = widgetFromThis<JZSplitter>(s, "setCollapsible"))
            split->setCollapsible(i, c);
    });
    bind_split.def("isCollapsible", [this](JZObjectPointer* s, int i) {
        JZSplitter* split = widgetFromThis<JZSplitter>(s, "isCollapsible");
        return split && split->isCollapsible(i);
    });
    defSignalProperty(bind_split, "onSplitterMoved", &JZSplitter::sigSplitterMoved);
    bind_split.setCreate(js_JZSplitter_ctor);
    mountTransparentClass(bind_split.classDefine());

    // ---- JZScrollArea: 视图容器(setWidget(content) 是唯一挂载入口; 对象形参 = JZObjectPointer*) ----
    jsbind::ClassBind<JZScrollArea> bind_area("JZScrollArea", bind_w);
    bind_area.def("setWidget", [this](JZObjectPointer* a, JZObjectPointer* content) {
        JZScrollArea* area = widgetFromThis<JZScrollArea>(a, "setWidget");
        JZWidget* c = widgetFromThis<JZWidget>(content, "setWidget");   // 传 null 已由形参层报错
        if (area && c)
            area->setWidget(c);
    });
    // 内容控件(setWidget 进来的 = JS 造的, 答得出) / 内部视口(C++ 造的, 从没被 JS 拿到过 ->
    // 返回 null; 要 JS 侧的对象得走 JS 便利层的登记表 —— 但那本来也没有对应 JS 对象)
    bind_area.def("widget", [this](JZObjectPointer* a) -> JSValue {
        JZScrollArea* area = widgetFromThis<JZScrollArea>(a, "widget");
        return area ? widgetJsView(area->widget()) : JS_EXCEPTION;
    });
    bind_area.def("viewport", [this](JZObjectPointer* a) -> JSValue {
        JZScrollArea* area = widgetFromThis<JZScrollArea>(a, "viewport");
        return area ? widgetJsView(area->viewport()) : JS_EXCEPTION;
    });
    bind_area.def("setScrollDirection", [this](JZObjectPointer* a, int d) {
        if (JZScrollArea* area = widgetFromThis<JZScrollArea>(a, "setScrollDirection"))
            area->setScrollDirection(d);
    });
    bind_area.def("setScrollBarPolicy", [this](JZObjectPointer* a, int p) {
        if (JZScrollArea* area = widgetFromThis<JZScrollArea>(a, "setScrollBarPolicy"))
            area->setScrollBarPolicy((JZScrollArea::ScrollBarPolicy)p);
    });
    bind_area.def("setVerticalScrollBarPolicy", [this](JZObjectPointer* a, int p) {
        if (JZScrollArea* area = widgetFromThis<JZScrollArea>(a, "setVerticalScrollBarPolicy"))
            area->setVerticalScrollBarPolicy((JZScrollArea::ScrollBarPolicy)p);
    });
    bind_area.def("setHorizontalScrollBarPolicy", [this](JZObjectPointer* a, int p) {
        if (JZScrollArea* area = widgetFromThis<JZScrollArea>(a, "setHorizontalScrollBarPolicy"))
            area->setHorizontalScrollBarPolicy((JZScrollArea::ScrollBarPolicy)p);
    });
    bind_area.def("setContentSize", [this](JZObjectPointer* a, int w, int h) {
        if (JZScrollArea* area = widgetFromThis<JZScrollArea>(a, "setContentSize"))
            area->setContentSize(w, h);
    });
    bind_area.def("contentWidth", [this](JZObjectPointer* a) {
        JZScrollArea* area = widgetFromThis<JZScrollArea>(a, "contentWidth");
        return area ? area->contentWidth() : 0;
    });
    bind_area.def("contentHeight", [this](JZObjectPointer* a) {
        JZScrollArea* area = widgetFromThis<JZScrollArea>(a, "contentHeight");
        return area ? area->contentHeight() : 0;
    });
    bind_area.def("viewportWidth", [this](JZObjectPointer* a) {
        JZScrollArea* area = widgetFromThis<JZScrollArea>(a, "viewportWidth");
        return area ? area->viewportWidth() : 0;
    });
    bind_area.def("viewportHeight", [this](JZObjectPointer* a) {
        JZScrollArea* area = widgetFromThis<JZScrollArea>(a, "viewportHeight");
        return area ? area->viewportHeight() : 0;
    });
    bind_area.def("scrollX", [this](JZObjectPointer* a) {
        JZScrollArea* area = widgetFromThis<JZScrollArea>(a, "scrollX");
        return area ? area->scrollX() : 0;
    });
    bind_area.def("scrollY", [this](JZObjectPointer* a) {
        JZScrollArea* area = widgetFromThis<JZScrollArea>(a, "scrollY");
        return area ? area->scrollY() : 0;
    });
    bind_area.def("maxScrollX", [this](JZObjectPointer* a) {
        JZScrollArea* area = widgetFromThis<JZScrollArea>(a, "maxScrollX");
        return area ? area->maxScrollX() : 0;
    });
    bind_area.def("maxScrollY", [this](JZObjectPointer* a) {
        JZScrollArea* area = widgetFromThis<JZScrollArea>(a, "maxScrollY");
        return area ? area->maxScrollY() : 0;
    });
    bind_area.def("scrollTo", [this](JZObjectPointer* a, int x, int y) {
        if (JZScrollArea* area = widgetFromThis<JZScrollArea>(a, "scrollTo"))
            area->scrollTo(x, y);
    });
    bind_area.def("scrollToX", [this](JZObjectPointer* a, int x) {
        if (JZScrollArea* area = widgetFromThis<JZScrollArea>(a, "scrollToX"))
            area->scrollToX(x);
    });
    bind_area.def("scrollToY", [this](JZObjectPointer* a, int y) {
        if (JZScrollArea* area = widgetFromThis<JZScrollArea>(a, "scrollToY"))
            area->scrollToY(y);
    });
    bind_area.def("scrollToTop", [this](JZObjectPointer* a) {
        if (JZScrollArea* area = widgetFromThis<JZScrollArea>(a, "scrollToTop"))
            area->scrollToTop();
    });
    bind_area.def("scrollToBottom", [this](JZObjectPointer* a) {
        if (JZScrollArea* area = widgetFromThis<JZScrollArea>(a, "scrollToBottom"))
            area->scrollToBottom();
    });
    bind_area.def("canScrollVertically", [this](JZObjectPointer* a) {
        JZScrollArea* area = widgetFromThis<JZScrollArea>(a, "canScrollVertically");
        return area && area->canScrollVertically();
    });
    bind_area.def("canScrollHorizontally", [this](JZObjectPointer* a) {
        JZScrollArea* area = widgetFromThis<JZScrollArea>(a, "canScrollHorizontally");
        return area && area->canScrollHorizontally();
    });
    bind_area.setCreate(js_JZScrollArea_ctor);
    mountTransparentClass(bind_area.classDefine());

    // ---- JZScrollBar: 一般由 JZScrollArea 内部持有, 也允许单独用 ----
    // 注: 构造实参 (orientation, parent); 取方向用整数(枚举值) —— 与 JZSplitter.orientation 一致。
    jsbind::ClassBind<JZScrollBar> bind_sbar("JZScrollBar", bind_w);
    bind_sbar.def("setOrientation", [this](JZObjectPointer* b, int o) {
        if (JZScrollBar* bar = widgetFromThis<JZScrollBar>(b, "setOrientation"))
            bar->setOrientation((JZScrollBar::Orientation)o);
    });
    bind_sbar.def("orientation", [this](JZObjectPointer* b) {
        JZScrollBar* bar = widgetFromThis<JZScrollBar>(b, "orientation");
        return bar ? (int)bar->orientation() : 0;
    });
    bind_sbar.def("setRange", [this](JZObjectPointer* b, int lo, int hi) {
        if (JZScrollBar* bar = widgetFromThis<JZScrollBar>(b, "setRange"))
            bar->setRange(lo, hi);
    });
    bind_sbar.def("setMinimum", [this](JZObjectPointer* b, int v) {
        if (JZScrollBar* bar = widgetFromThis<JZScrollBar>(b, "setMinimum"))
            bar->setMinimum(v);
    });
    bind_sbar.def("minimum", [this](JZObjectPointer* b) {
        JZScrollBar* bar = widgetFromThis<JZScrollBar>(b, "minimum");
        return bar ? bar->minimum() : 0;
    });
    bind_sbar.def("setMaximum", [this](JZObjectPointer* b, int v) {
        if (JZScrollBar* bar = widgetFromThis<JZScrollBar>(b, "setMaximum"))
            bar->setMaximum(v);
    });
    bind_sbar.def("maximum", [this](JZObjectPointer* b) {
        JZScrollBar* bar = widgetFromThis<JZScrollBar>(b, "maximum");
        return bar ? bar->maximum() : 0;
    });
    bind_sbar.def("setValue", [this](JZObjectPointer* b, int v) {
        if (JZScrollBar* bar = widgetFromThis<JZScrollBar>(b, "setValue"))
            bar->setValue(v);
    });
    bind_sbar.def("value", [this](JZObjectPointer* b) {
        JZScrollBar* bar = widgetFromThis<JZScrollBar>(b, "value");
        return bar ? bar->value() : 0;
    });
    bind_sbar.def("setSingleStep", [this](JZObjectPointer* b, int v) {
        if (JZScrollBar* bar = widgetFromThis<JZScrollBar>(b, "setSingleStep"))
            bar->setSingleStep(v);
    });
    bind_sbar.def("singleStep", [this](JZObjectPointer* b) {
        JZScrollBar* bar = widgetFromThis<JZScrollBar>(b, "singleStep");
        return bar ? bar->singleStep() : 0;
    });
    bind_sbar.def("setPageStep", [this](JZObjectPointer* b, int v) {
        if (JZScrollBar* bar = widgetFromThis<JZScrollBar>(b, "setPageStep"))
            bar->setPageStep(v);
    });
    bind_sbar.def("pageStep", [this](JZObjectPointer* b) {
        JZScrollBar* bar = widgetFromThis<JZScrollBar>(b, "pageStep");
        return bar ? bar->pageStep() : 0;
    });
    defSignalProperty(bind_sbar, "onValueChanged", &JZScrollBar::sigValueChanged);
    bind_sbar.setCreate(js_JZScrollBar_ctor);
    mountTransparentClass(bind_sbar.classDefine());

    // ---- JZImageLabel: 单图显示(图像按值进, 读回的是拷贝) ----
    jsbind::ClassBind<JZImageLabel> bind_imglab("JZImageLabel", bind_w);
    bind_imglab.def("setImage", [this](JZObjectPointer* l, const JZImage& img) {
        if (JZImageLabel* lab = widgetFromThis<JZImageLabel>(l, "setImage"))
            lab->setImage(img);
    });
    bind_imglab.def("setScale", [this](JZObjectPointer* l, int zoom) {
        JZImageLabel* lab = widgetFromThis<JZImageLabel>(l, "setScale");
        if (!lab)
            return;
        if (zoom < 0 || zoom > 0xFFFF)
            throw std::runtime_error("JZImageLabel.setScale: 缩放倍数需要在 0~65535 之间");
        lab->setScale((uint16_t)zoom);
    });
    bind_imglab.def("setRotation", [this](JZObjectPointer* l, int angle) {
        if (JZImageLabel* lab = widgetFromThis<JZImageLabel>(l, "setRotation"))
            lab->setRotation(angle);
    });
    bind_imglab.def("setPivot", [this](JZObjectPointer* l, int x, int y) {
        if (JZImageLabel* lab = widgetFromThis<JZImageLabel>(l, "setPivot"))
            lab->setPivot(x, y);
    });
    bind_imglab.setCreate(js_JZImageLabel_ctor);
    mountTransparentClass(bind_imglab.classDefine());

    // 注(2026-09-23 更新): 需要回吐"控件指针"的 getter 现在都绑了 ——
    //   JZStackedWidget.widget(i)/currentWidget()/setCurrentWidget(w),
    //   JZTabWidget.widget(i)/currentWidget()/setCurrentWidget(w),
    //   JZSplitter.widget(i), JZScrollArea.widget()/viewport()。
    // 做法见 widgetJsView: 控件族的 JS 对象仍是"JS 侧造的", 但每个被 JS 包装过的控件都在
    // 对象动态属性 js_value 上留了**宿主句柄**, 于是 C++ 拿 JZWidget* 就能还回**同一个** JS 对象
    // (`===` 成立, 不再需要另立身份表)。只有"从没被 JS 拿到过"的控件(如 JZScrollArea 内部视口、
    // C++ 内部造的页)返回 null —— 那种情况本来也没有 JS 对象可回吐。
    // 另: 仍在 JS 便利层(tree.ts)登记关系的是**保活**(父强引用子包装), 与本层互补, 不重复。
}

// ---------- 布局 ----------

// 盒子布局的公共方法(JZVBoxLayout / JZHBoxLayout 各注册一份)。
// **形参必须写具体布局类**: JZBoxLayout 本身没有注册到 JS(纯虚 flexFlow ⇒ 无法默认构造),
// 用 `JZBoxLayout*` 当形参时 ctypeid 解析不到 ⇒ 形参类型退化成 JSType_none ⇒
// 每次调用都抛"参数0需要类型none，输入是JZVBoxLayout"(整个盒子布局在 JS 侧不可用)。
// 同理: 任何 def 的首个形参都必须是**已注册**的类。
template <class Layout>
static void defBoxLayoutMethods(jsbind::ClassBind<Layout>& bind, JZJSWidgetModule* module)
{
    // addWidget/insertWidget 收 JSValue: 子控件进布局后由容器对象树负责 delete, 并且要登记到
    // 父控件的控件表里(保活它的 JS 包装) —— 两件事都在 module->takeWidgetFromJs 里做。
    // 父控件 = 布局的容器(apply 时记下)。尚未 setLayout 时容器为空: 此时只能转移所有权,
    // 登记等 apply 把控件挂到容器下之后再说(那种写法请改用 w.layout().addWidget(...))。
    bind.def("addWidget", [module](Layout* l, JSValue w, int stretch) {        
        JZWidget* child = module->takeWidgetFromJs(w);
        l->addWidget(child, stretch);
    });
    bind.def("addSpacing", [](Layout* l, int s) { l->addSpacing(s); });
    bind.def("addStretch", [](Layout* l, int s) { l->addStretch(s); });
    bind.def("insertWidget", [module](Layout* l, int i, JSValue w, int stretch) {
        JZWidget* child = module->takeWidgetFromJs(w);
        l->insertWidget(i, child, stretch);
    });
    bind.def("removeAt", [](Layout* l, int i) { l->removeAt(i); });
    // 注: 这处在自由函数里(没有 this), 所以不用 widgetFromThis —— 自己做 dynamic_cast;
    // 形参层(引擎)已经保证"是个活着的对象指针", 这里只管"是不是控件"(不符抛异常 -> JS 抛出错)
    bind.def("removeWidget", [](Layout* l, JZObjectPointer* w) {
        JZObject* object = w ? w->object() : nullptr;
        JZWidget* child = object ? dynamic_cast<JZWidget*>(object) : nullptr;
        if (!child)
            throw std::runtime_error("JZLayout.removeWidget: 需要控件");
        l->removeWidget(child);
    });
    bind.def("clear", [](Layout* l) { l->clear(); });
    bind.def("count", [](Layout* l) { return l->count(); });
}

void JZJSWidgetModule::registLayouts()
{
    // 布局基类: 抽象, 不能 new(JS 侧 new JZLayout() 会抛; 具体布局见下)
    jsbind::ClassBind<JZLayout> bind_layout("JZLayout");
    bind_layout.def("setSpacing", [](JZLayout* l, int g) { l->setSpacing(g); });
    bind_layout.def("spacing", [](JZLayout* l) { return l->spacing(); });
    bind_layout.def("setContentsMargins", [](JZLayout* l, int l_, int t, int r, int b) { l->setContentsMargins(l_, t, r, b); });
    m_engine->registClass(bind_layout.classDefine());

    // 盒子布局 (JZVBoxLayout / JZHBoxLayout) — 方法见上方 defBoxLayoutMethods;
    // setSpacing/spacing/setContentsMargins 由 JZLayout 那份提供(原型链继承, 形参类型正确)。
    // 注意: 子布局必须把**基类**一起注册(bind_layout), 否则:
    //   ① JS 侧 instanceof JZLayout 为假; ② 框架里 setLayout(JZLayout*) 这类签名
    //   会按 JS 类层次做参数类型检查, 直接把 JZGridLayout 判成"不是 JZLayout"而报错
    //   (参数1需要类型JZLayout,输入是JZGridLayout)。
    jsbind::ClassBind<JZVBoxLayout> bind_vbox("JZVBoxLayout", bind_layout);
    defBoxLayoutMethods(bind_vbox, this);
    bind_vbox.setCreate(js_JZVBoxLayout_ctor);
    m_engine->registClass(bind_vbox.classDefine());
    
    jsbind::ClassBind<JZHBoxLayout> bind_hbox("JZHBoxLayout", bind_layout);
    defBoxLayoutMethods(bind_hbox, this);
    bind_hbox.setCreate(js_JZHBoxLayout_ctor);
    m_engine->registClass(bind_hbox.classDefine());

    // JZGridLayout
    jsbind::ClassBind<JZGridLayout> bind_grid("JZGridLayout", bind_layout);
    // 父控件 = 布局的容器(尚未 setLayout 时为空, 那种写法只能转所有权); 与盒子布局同一套
    bind_grid.def("addWidget", [this](JZGridLayout* l, JSValue w, int row, int col) {
        JZWidget* child = takeWidgetFromJs(w);
        if (child)
            l->addWidget(child, row, col);
    });
    bind_grid.def("addWidgetSpan", [this](JZGridLayout* l, JSValue w, int r, int c, int rs, int cs) {
        JZWidget* child = takeWidgetFromJs(w);
        if (child)
            l->addWidget(child, r, c, rs, cs);
    });
    bind_grid.def("setRowStretch", [](JZGridLayout* l, int row, int s) { l->setRowStretch(row, s); });
    bind_grid.def("setColumnStretch", [](JZGridLayout* l, int col, int s) { l->setColumnStretch(col, s); });
    bind_grid.def("setRowMinimumHeight", [](JZGridLayout* l, int row, int h) { l->setRowMinimumHeight(row, h); });
    bind_grid.def("setColumnMinimumWidth", [](JZGridLayout* l, int col, int w) { l->setColumnMinimumWidth(col, w); });
    bind_grid.def("setHorizontalSpacing", [](JZGridLayout* l, int s) { l->setHorizontalSpacing(s); });
    bind_grid.def("setVerticalSpacing", [](JZGridLayout* l, int s) { l->setVerticalSpacing(s); });
    bind_grid.def("rowCount", [](JZGridLayout* l) { return l->rowCount(); });
    bind_grid.def("columnCount", [](JZGridLayout* l) { return l->columnCount(); });
    bind_grid.def("count", [](JZGridLayout* l) { return l->count(); });
    bind_grid.def("removeWidget", [this](JZGridLayout* l, JZObjectPointer* w) {
        if (JZWidget* child = widgetFromThis<JZWidget>(w, "removeWidget"))
            l->removeWidget(child);
    });
    bind_grid.def("clear", [](JZGridLayout* l) { l->clear(); });
    bind_grid.setCreate(js_JZGridLayout_ctor);
    m_engine->registClass(bind_grid.classDefine());

    // JZSelfLayout
    jsbind::ClassBind<JZSelfLayout> bind_self("JZSelfLayout");    
    m_engine->registClass(bind_self.classDefine());
}

// ---------- JZPainter 的画笔 / 画刷 / 字体 ----------
//
// 两个形参形态都收(对象优先; 便捷形式是给 Qt 兼容层/老代码留的, 见下):
//   setPen(pen: JZPen)                       —— 完整描述(颜色 + 线宽), 推荐
//   setPen(color, width?)                    —— color: '#rrggbb' | 颜色名 | JZColor
//   setBrush(brush: JZBrush) / setBrush(color, opacity?)
//   setFont(font: JZFont)    / setFont(family, size)
//
// 为什么做成 quickjs C 函数(而不是 def 的 lambda): 引擎的 def 走 JZJSEngine::call,
// **参数个数必须精确匹配**形参表 —— 一个 lambda 表达不了"可选参数"这种形态;
// C 函数拿到原始 argc/argv, 可以自己按实参形态分派并给出明确报错(与 js_JZWidget_* 同一套路)。

// this_val -> JZPainter*(非本类对象 / 包装已失效一律 nullptr, 由调用方报错)
static JZPainter* jzPainterFromThis(JSContext* ctx, JSValueConst this_val)
{
    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    if (!engine)
        return nullptr;

    JSClassID class_id = 0;
    void* opaque = JS_GetAnyOpaque(this_val, &class_id);
    if (!js_check_class_arg<JZPainter>(ctx, this_val))
        return nullptr;

    return fromJSValue<JZPainter*>(ctx, this_val);
}

// 整数形参(自行校验: C 函数不经过形参类型表)
static bool jzJsToIntArg(JSContext* ctx, JSValueConst v, int& out, const char* what, const char* arg)
{
    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    if (!engine || !engine->checkType(v, JSType_int))
    {
        JS_ThrowTypeError(ctx, "%s: %s 需要整数", what, arg);
        return false;
    }
    out = fromJSValue<int>(ctx, v);
    return true;
}

// 数字形参(自行校验: C 函数不经过形参类型表)
static bool jzJsToDoubleArg(JSContext* ctx, JSValueConst v, double& out, const char* what, const char* arg)
{
    if (!JS_IsNumber(v) || JS_ToFloat64(ctx, &out, v) != 0)
    {
        JS_ThrowTypeError(ctx, "%s: %s 需要数字", what, arg);
        return false;
    }
    return true;
}

// 颜色形参: JZColor 对象 或 '#rrggbb'/颜色名 字符串
static bool jzJsToColorArg(JSContext* ctx, JSValueConst v, JZColor& out, const char* what)
{
    if (JS_IsString(v))
    {
        out = jzParseColor(fromJSValue<std::string>(ctx, v));
        return true;
    }
    if (js_check_class_arg<JZColor>(ctx, v))
    {
        out = *fromJSValue<JZColor*>(ctx, v);
        return true;
    }
    JS_ThrowTypeError(ctx, "%s: 颜色需要 '#rrggbb'/颜色名 字符串或 JZColor 对象", what);
    return false;
}

static JSValue js_JZPainter_setPen(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* what = "JZPainter.setPen";
    JZPainter* p = jzPainterFromThis(ctx, this_val);
    if (!p)
        return JS_ThrowTypeError(ctx, "%s: this 不是有效的 JZPainter", what);
    if (argc < 1 || argc > 2)
        return JS_ThrowTypeError(ctx, "%s: 需要 setPen(pen) 或 setPen(color, width?)", what);

    if (js_check_class_arg<JZPen>(ctx, argv[0]))
    {
        if (argc != 1)
            return JS_ThrowTypeError(ctx, "%s(pen): 线宽请写进 JZPen, 不要再给第 2 个参数", what);
        p->setPen(*fromJSValue<JZPen*>(ctx, argv[0]));
        return JS_UNDEFINED;
    }

    int width = 1;
    if (argc == 2 && !jzJsToIntArg(ctx, argv[1], width, what, "width"))
        return JS_EXCEPTION;
    JZColor color;
    if (!jzJsToColorArg(ctx, argv[0], color, what))
        return JS_EXCEPTION;
    p->setPen(JZPen(color, width));
    return JS_UNDEFINED;
}

static JSValue js_JZPainter_setBrush(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* what = "JZPainter.setBrush";
    JZPainter* p = jzPainterFromThis(ctx, this_val);
    if (!p)
        return JS_ThrowTypeError(ctx, "%s: this 不是有效的 JZPainter", what);
    if (argc < 1 || argc > 2)
        return JS_ThrowTypeError(ctx, "%s: 需要 setBrush(brush) 或 setBrush(color, opacity?)", what);

    if (js_check_class_arg<JZBrush>(ctx, argv[0]))
    {
        if (argc != 1)
            return JS_ThrowTypeError(ctx, "%s(brush): 不透明度请写进 JZBrush, 不要再给第 2 个参数", what);
        p->setBrush(*fromJSValue<JZBrush*>(ctx, argv[0]));
        return JS_UNDEFINED;
    }

    uint8_t opacity = JZBrush::kOpaCover;
    if (argc == 2)
    {
        int n = 0;
        if (!jzJsToIntArg(ctx, argv[1], n, what, "opacity"))
            return JS_EXCEPTION;
        if (n < 0 || n > 255)
            return JS_ThrowTypeError(ctx, "%s: opacity 需要在 0~255 之间", what);
        opacity = (uint8_t)n;
    }
    JZColor color;
    if (!jzJsToColorArg(ctx, argv[0], color, what))
        return JS_EXCEPTION;
    p->setBrush(JZBrush(color, opacity));
    return JS_UNDEFINED;
}

static JSValue js_JZPainter_setFont(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* what = "JZPainter.setFont";
    JZPainter* p = jzPainterFromThis(ctx, this_val);
    if (!p)
        return JS_ThrowTypeError(ctx, "%s: this 不是有效的 JZPainter", what);
    if (argc < 1 || argc > 2)
        return JS_ThrowTypeError(ctx, "%s: 需要 setFont(font) 或 setFont(family, size)", what);

    if (js_check_class_arg<JZFont>(ctx, argv[0]))
    {
        if (argc != 1)
            return JS_ThrowTypeError(ctx, "%s(font): 不要再给第 2 个参数(字号写进 JZFont)", what);
        p->setFont(*fromJSValue<JZFont*>(ctx, argv[0]));
        return JS_UNDEFINED;
    }

    if (!JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "%s: 需要 JZFont 对象或字体族字符串", what);
    if (argc != 2)
        return JS_ThrowTypeError(ctx, "%s(family, size): 用字符串时还需要字号", what);

    std::string family = fromJSValue<std::string>(ctx, argv[0]);
    int size = 0;
    if (!jzJsToIntArg(ctx, argv[1], size, what, "size"))
        return JS_EXCEPTION;
    p->setFont(JZFont(family, size));
    return JS_UNDEFINED;
}

static JSValue js_JZPainter_setTransform(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* what = "JZPainter.setTransform";
    JZPainter* p = jzPainterFromThis(ctx, this_val);
    if (!p)
        return JS_ThrowTypeError(ctx, "%s: this 不是有效的 JZPainter", what);

    // 对象形式(推荐): 直接用 JZMatrix2D
    if (argc == 1)
    {
        if (!js_check_class_arg<JZMatrix2D>(ctx, argv[0]))
            return JS_ThrowTypeError(ctx, "%s: 需要 JZMatrix2D 对象", what);
        p->setTransform(*fromJSValue<JZMatrix2D*>(ctx, argv[0]));
        return JS_UNDEFINED;
    }

    // 便捷形式: 6 个数 = 2x3 仿射部分(行主序 [a b c d e f], 与 GDI+/Canvas 一致)
    if (argc == 6)
    {
        double v[6];
        for (int i = 0; i < 6; i++)
        {
            if (!jzJsToDoubleArg(ctx, argv[i], v[i], what, "a~f"))
                return JS_EXCEPTION;
        }
        JZMatrix2D m;
        m.setMatrix(v[0], v[1], v[2], v[3], v[4], v[5]);
        p->setTransform(m);
        return JS_UNDEFINED;
    }

    return JS_ThrowTypeError(ctx, "%s: 需要 setTransform(matrix) 或 setTransform(a, b, c, d, e, f)", what);
}

// ---------- 2D 变换矩阵 (JZMatrix2D) ----------
//
// 值语义的 2x3 仿射矩阵(行主序 [a b c d e f]): 传给 JZPainter.setTransform(m),
// 也可用静态工厂造: JZMatrix2D.translation(dx, dy) / scaling(sx, sy) / rotation(deg)。
// 组合语义与 C++ 一致(post-multiply): 每次 translate/rotate/scale 都在"已变换的坐标系内"继续。

// 静态工厂(挂构造器上, 见 registMatrix 的 defStaticFunction): 各造一个新矩阵返回。
static JSValue js_JZMatrix2D_translation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    const char* what = "JZMatrix2D.translation(dx, dy)";
    if (argc != 2)
        return JS_ThrowTypeError(ctx, "%s: 需要 2 个参数", what);
    double dx = 0, dy = 0;
    if (!jzJsToDoubleArg(ctx, argv[0], dx, what, "dx") || !jzJsToDoubleArg(ctx, argv[1], dy, what, "dy"))
        return JS_EXCEPTION;
    return toJSValue(ctx, JZMatrix2D::translation(dx, dy));
}

static JSValue js_JZMatrix2D_scaling(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    const char* what = "JZMatrix2D.scaling(sx, sy)";
    if (argc != 2)
        return JS_ThrowTypeError(ctx, "%s: 需要 2 个参数", what);
    double sx = 1, sy = 1;
    if (!jzJsToDoubleArg(ctx, argv[0], sx, what, "sx") || !jzJsToDoubleArg(ctx, argv[1], sy, what, "sy"))
        return JS_EXCEPTION;
    return toJSValue(ctx, JZMatrix2D::scaling(sx, sy));
}

static JSValue js_JZMatrix2D_rotation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    const char* what = "JZMatrix2D.rotation(deg)";
    if (argc != 1)
        return JS_ThrowTypeError(ctx, "%s: 需要 1 个参数", what);
    double deg = 0;
    if (!jzJsToDoubleArg(ctx, argv[0], deg, what, "deg"))
        return JS_EXCEPTION;
    return toJSValue(ctx, JZMatrix2D::rotation(deg));
}

static JSValue js_JZMatrix2D_rotationRadians(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    const char* what = "JZMatrix2D.rotationRadians(rad)";
    if (argc != 1)
        return JS_ThrowTypeError(ctx, "%s: 需要 1 个参数", what);
    double rad = 0;
    if (!jzJsToDoubleArg(ctx, argv[0], rad, what, "rad"))
        return JS_EXCEPTION;
    return toJSValue(ctx, JZMatrix2D::rotationRadians(rad));
}

void JZJSWidgetModule::registMatrix()
{
    jsbind::ClassBind<JZMatrix2D> bind_m("JZMatrix2D");
    bind_m.def("reset", [](JZMatrix2D* m) { m->reset(); });         // 置单位阵
    bind_m.def("isIdentity", [](JZMatrix2D* m) { return m->isIdentity(); });
    bind_m.def("at", [](JZMatrix2D* m, int row, int col) {
        if (row < 0 || row > 2 || col < 0 || col > 2)
            throw std::runtime_error("JZMatrix2D.at: 行列下标需要在 0~2 之间");
        return m->at(row, col);
    });
    bind_m.def("setAt", [](JZMatrix2D* m, int row, int col, double v) {
        if (row < 0 || row > 2 || col < 0 || col > 2)
            throw std::runtime_error("JZMatrix2D.setAt: 行列下标需要在 0~2 之间");
        m->at(row, col) = v;
    });
    bind_m.def("setMatrix", [](JZMatrix2D* m, double a, double b, double c, double d, double e, double f) {
        m->setMatrix(a, b, c, d, e, f);
    });
    bind_m.def("setMatrix3", [](JZMatrix2D* m, double m00, double m01, double m02,
                                                 double m10, double m11, double m12,
                                                 double m20, double m21, double m22) {
        m->setMatrix3(m00, m01, m02, m10, m11, m12, m20, m21, m22);
    });
    bind_m.def("translate", [](JZMatrix2D* m, double dx, double dy) { m->translate(dx, dy); });
    bind_m.def("scale", [](JZMatrix2D* m, double sx, double sy) { m->scale(sx, sy); });
    /** 角度制, 屏幕顺时针为正 */
    bind_m.def("rotate", [](JZMatrix2D* m, double deg) { m->rotate(deg); });
    bind_m.def("rotateRadians", [](JZMatrix2D* m, double rad) { m->rotateRadians(rad); });
    /** this = this * o (先应用 o, 再应用 this) */
    bind_m.def("multiply", [](JZMatrix2D* m, const JZMatrix2D& o) { m->multiply(o); });
    /** 不改本对象, 返回 this * o 的新矩阵 */
    bind_m.def("multiplied", [](JZMatrix2D* m, const JZMatrix2D& o) { return m->multiplied(o); });
    /** 仿射逆矩阵; 退化(det≈0)时返回单位阵 */
    bind_m.def("inverse", [](JZMatrix2D* m) { return m->inverse(); });
    bind_m.def("scaleX", [](JZMatrix2D* m) { return m->scaleX(); });
    bind_m.def("scaleY", [](JZMatrix2D* m) { return m->scaleY(); });
    bind_m.setCreate(js_JZMatrix2D_ctor);
    m_engine->registClass(bind_m.classDefine());

    // 静态工厂(挂构造器上, 与 JZImage.create 同一机制): 造一个**新矩阵**返回。
    defStaticFunction(bind_m.classDefine(), "translation", js_JZMatrix2D_translation, 2);
    defStaticFunction(bind_m.classDefine(), "scaling", js_JZMatrix2D_scaling, 2);
    defStaticFunction(bind_m.classDefine(), "rotation", js_JZMatrix2D_rotation, 1);
    defStaticFunction(bind_m.classDefine(), "rotationRadians", js_JZMatrix2D_rotationRadians, 1);
}

// ---------- 画笔样式值类型 (JZPen / JZBrush / JZFont) ----------

// 注册到 JS 供 JZPainter.setPen/setBrush/setFont 传对象(构造器换 C 版本见 JZJSWidget.cpp)。
// 三个都是**值语义**描述: 读取接口按值返回拷贝(改它不影响画笔里那份)。
void JZJSWidgetModule::registPens()
{
    jsbind::ClassBind<JZPen> bind_pen("JZPen");
    bind_pen.def("setColor", [](JZPen* p, const JZColor& c) { p->setColor(c); });
    bind_pen.def("color", [](JZPen* p) { return p->color(); });
    bind_pen.def("setWidth", [](JZPen* p, int w) { p->setWidth(w); });
    bind_pen.def("width", [](JZPen* p) { return p->width(); });
    bind_pen.setCreate(js_JZPen_ctor);
    m_engine->registClass(bind_pen.classDefine());

    jsbind::ClassBind<JZBrush> bind_brush("JZBrush");
    bind_brush.def("setColor", [](JZBrush* b, const JZColor& c) { b->setColor(c); });
    bind_brush.def("color", [](JZBrush* b) { return b->color(); });
    bind_brush.def("setOpacity", [](JZBrush* b, int o) {
        if (o < 0 || o > 255)
            throw std::runtime_error("JZBrush.setOpacity: 不透明度需要在 0~255 之间");
        b->setOpacity((uint8_t)o);
    });
    bind_brush.def("opacity", [](JZBrush* b) { return (int)b->opacity(); });
    bind_brush.setCreate(js_JZBrush_ctor);
    m_engine->registClass(bind_brush.classDefine());

    jsbind::ClassBind<JZFont> bind_font("JZFont");
    bind_font.def("setFamily", [](JZFont* f, const std::string& v) { f->setFamily(v); });
    bind_font.def("family", [](JZFont* f) { return f->family(); });
    bind_font.def("setSize", [](JZFont* f, int v) { f->setSize(v); });
    bind_font.def("size", [](JZFont* f) { return f->size(); });
    bind_font.def("setStyle", [](JZFont* f, int v) { f->setStyle(v); });
    bind_font.def("style", [](JZFont* f) { return f->style(); });
    bind_font.def("setBold", [](JZFont* f, bool on) { f->setBold(on); });
    bind_font.def("bold", [](JZFont* f) { return f->bold(); });
    bind_font.def("setItalic", [](JZFont* f, bool on) { f->setItalic(on); });
    bind_font.def("italic", [](JZFont* f) { return f->italic(); });
    bind_font.def("setUnderline", [](JZFont* f, bool on) { f->setUnderline(on); });
    bind_font.def("underline", [](JZFont* f) { return f->underline(); });
    bind_font.def("setStrikeOut", [](JZFont* f, bool on) { f->setStrikeOut(on); });
    bind_font.def("strikeOut", [](JZFont* f) { return f->strikeOut(); });
    bind_font.def("equal", [](JZFont* f, const JZFont& o) { return *f == o; });   // C++ 侧本来就有 operator==
    // ---- 字体度量 (等价 Qt QFontMetrics::width/height; 原 C++ 侧由 JZFontMetrics 门面提供) ----
    // 自绘控件排版用: 后端(GDI+)注册了度量引擎就是**真度量**, 否则按 UTF-8 估算
    // (ASCII ~0.55em / CJK 1em, 见 JZFontMetrics::estimateWidth)。JS 侧典型用法:
    //   const f = painter.font();            // 屏幕上真正在用的那份字体
    //   const w = f.measureText(text);       // 真宽度(像素)
    //   painter.drawText(x + (cw - w) / 2, y, text);
    bind_font.def("measureText", [](JZFont* f, const std::string& text) { return JZFontMetrics::textWidth(text, *f); });
    bind_font.def("textWidth", [](JZFont* f, const std::string& text) { return JZFontMetrics::textWidth(text, *f); });
    bind_font.def("textHeight", [](JZFont* f) { return JZFontMetrics::textHeight(*f); });
    bind_font.def("ascent", [](JZFont* f) { return JZFontMetrics::ascent(*f); });
    bind_font.def("descent", [](JZFont* f) { return JZFontMetrics::descent(*f); });
    bind_font.setCreate(js_JZFont_ctor);
    m_engine->registClass(bind_font.classDefine());

    // 风格位常量挂到 JZFont 构造器上(JS 侧写 JZFont.Bold / JZFont.Italic ...), 与 C++ 枚举同值。
    // 注: 构造器下面还会被 jzRegistWidgetJsConstructors 换成 C 版本, 那次替换会把旧构造器上的
    //     自有可枚举属性一并搬过去(同 JZChart 的类型常量), 所以这里挂的常量不会丢。
    {
        int font_type_id = m_engine->ctypeidToType(typeid(JZFont).name());
        if (font_type_id != JSType_none)
        {
            JSContext* ctx = m_engine->context();
            JSValue js_class = m_engine->classConstructor(font_type_id);
            JS_SetPropertyStr(ctx, js_class, "Normal", JS_NewInt32(ctx, (int)JZFont::Normal));
            JS_SetPropertyStr(ctx, js_class, "Bold", JS_NewInt32(ctx, (int)JZFont::Bold));
            JS_SetPropertyStr(ctx, js_class, "Italic", JS_NewInt32(ctx, (int)JZFont::Italic));
            JS_SetPropertyStr(ctx, js_class, "Underline", JS_NewInt32(ctx, (int)JZFont::Underline));
            JS_SetPropertyStr(ctx, js_class, "StrikeOut", JS_NewInt32(ctx, (int)JZFont::StrikeOut));
            JS_FreeValue(ctx, js_class);
        }
    }
}

// ---------- JZPainter (绘制类, 非事件转发) ----------

// end() 的托管侧收尾: view 就是本次派发交给 JS 的那份屏幕画笔吗? 是就摘托管(底层对象交给调用方)。
JZPainter* JZJSWidgetModule::takePaintScopedPainter(JSValueConst view)
{
    if (!m_painter)
        return nullptr;
    if (JS_IsUndefined(m_paintView) || JS_VALUE_GET_PTR(m_paintView) != JS_VALUE_GET_PTR(view))
        return nullptr;   // 不是这个作用域的那份(离屏画笔由 JS 拥有)

    JZPainter* painter = m_painter;
    m_painter = nullptr;   // 派发末尾的 endPainter() 就不会再 delete 它了

    if (m_engine)
    {
        m_engine->disarmOpaque(m_paintView);   // 这份视图立刻失效: 再调用抛"已失效"
        JS_FreeValue(m_engine->context(), m_paintView);
    }
    m_paintView = JS_UNDEFINED;
    return painter;
}

// JZPainter.end() —— 裸函数(quickjs 直接调用, 带 this_val):
//   * 屏幕画笔: 模块托管 -> 摘 m_painter + 作废视图 + 在这里 delete(重复 end() 安全, 派发末尾不再回收);
//   * 离屏画笔: JS 拥有 -> 立即合成回 image + 回收底层对象 + 作废视图(包装还留着, 但再调用会抛)。
static JSValue js_JZPainterEnd(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    JZJSWidgetModule* module = engine ? engine->widgetModule() : nullptr;

    JSClassID class_id = 0;
    void* opaque = JS_GetAnyOpaque(this_val, &class_id);
    if (!opaque)
        return JS_ThrowTypeError(ctx, "JZPainter.end(): 这一份画笔已经结束了");

    JZPainter* painter = (JZPainter*)opaque;
    painter->endDevice();

    // 屏幕画笔: 归模块所有 -> 摘托管 + 这里回收(take 里已经把这份视图作废了)
    JZPainter* scoped = module ? module->takePaintScopedPainter(this_val) : nullptr;
    if (scoped)
    {
        delete scoped;
        return JS_UNDEFINED;
    }

    // 离屏画笔: 归 JS 所有 -> 合成已完成, 底层对象在这里回收, 这份包装作废
    // (终结器见到空 opaque 就只放引用, 不会重复 delete)
    delete painter;
    if (engine)
        engine->disarmOpaque(this_val);
    return JS_UNDEFINED;
}

void JZJSWidgetModule::registPainter()
{
    jsbind::ClassBind<JZPainter> bind_p("JZPainter");
    // 创建请求全部转发给模块持有的 JZJSPaintScope(动态转发 + 登记 + 生命周期):
    //   new JZPainter(widget) -> 屏幕画笔, 等价 C++ 的栈对象 `JZPainter p(this)`, 只在本次 onPaint 内有效
    //   new JZPainter(image)  -> 离屏画笔, 自持图层, 析构(或 p.end())时把内容合成回 image
    // 登记/失效只留包装的**共享状态**(JSRawPointer<JZPainter>::Info), 不需要 setInstanceBind 之类的
    // 创建后钩子: create 阶段本就拿不到 JSValue, 也不必拿到 —— 状态独立于 JS 对象存活,
    // 释放点在 paintEvent 末尾(JZJSPaintScope::Guard → end()), 那里用状态就能完成 delete + 失效。
    // 包装模型: opaque 就是 JZPainter*(JZPainter 不是 JZObject, 没有守卫)。
    // 生命周期两种: ① 屏幕画笔(模块托管, 见 createPainter): 模块 delete, 派发结束作废视图;
    //              ② 离屏画笔(JS 拥有): JS 回收时由终结器 delete。
    bind_p.setCreate([this](JZJSEngine* ctx, int argc, JSValue* argv) -> void* 
    {
        return this->createPainter(ctx->context(), argc, argv);
    });
    // 显式结束绘制: 离屏模式立即把图层合成回 image(之后 image 才可用), 屏幕模式提前补 Pop。
    // 同时让**这一份**包装立刻失效 + 把画笔对象就地回收(见 js_JZPainterEnd)。
    // 裸函数(要 this): 才能作废"这一份"视图 —— def 的 lambda 拿不到 JS 对象。
    bind_p.def("end", &js_JZPainterEnd);
    bind_p.def("save", [](JZPainter* p) { p->save(); });
    bind_p.def("restore", [](JZPainter* p) { p->restore(); });
    bind_p.def("translate", [](JZPainter* p, double dx, double dy) { p->translate(dx, dy); });
    bind_p.def("rotate", [](JZPainter* p, double deg) { p->rotate(deg); });
    bind_p.def("scale", [](JZPainter* p, double sx, double sy) { p->scale(sx, sy); });
    bind_p.def("resetTransform", [](JZPainter* p) { p->resetTransform(); });
    bind_p.def("setClip", [](JZPainter* p, int x, int y, int w, int h) { p->setClip(JZRect(x, y, w, h)); });
    // 画笔/画刷/字体: 收 JZPen / JZBrush / JZFont 对象(也可用颜色字符串的便捷形式) ——
    // 见上面三个 C 函数的说明(参数个数按实参形态分派, 故不用 def 的 lambda)。
    bind_p.def("setPen", js_JZPainter_setPen, 1);
    bind_p.def("setBrush", js_JZPainter_setBrush, 1);
    bind_p.def("setFont", js_JZPainter_setFont, 1);
    // 当前字体(拷贝): 自绘控件要按"屏幕上真正用的那份字体"量文本宽度时读它,
    // 再交给 JZFont.measureText() —— 见 pen.d.ts 的说明。
    bind_p.def("font", [](JZPainter* p) { return p->font(); });
    bind_p.def("drawLine", [](JZPainter* p, int x1, int y1, int x2, int y2) { p->drawLine(x1, y1, x2, y2); });
    bind_p.def("drawRect", [](JZPainter* p, int x, int y, int w, int h) { p->drawRect(x, y, w, h); });
    bind_p.def("drawRoundRect", [](JZPainter* p, int x, int y, int w, int h, int r) { p->drawRoundRect(x, y, w, h, r); });
    bind_p.def("fillRect", [](JZPainter* p, int x, int y, int w, int h, const std::string& color) { p->fillRect(x, y, w, h, jzParseColor(color)); });
    bind_p.def("fillRoundRect", [](JZPainter* p, int x, int y, int w, int h, int r, const std::string& color) { p->fillRoundRect(x, y, w, h, r, jzParseColor(color)); });
    bind_p.def("drawEllipse", [](JZPainter* p, int x, int y, int w, int h) { p->drawEllipse(x, y, w, h); });
    bind_p.def("fillEllipse", [](JZPainter* p, int x, int y, int w, int h, const std::string& color) { p->fillEllipse(x, y, w, h, jzParseColor(color)); });
    // 画弧: x/y 是**圆心**(不是左上角), 角度单位为 1/10 度(0=3点方向, 顺时针递增),
    // 整圆写 drawArc(cx, cy, r, 0, 3600) —— 与 C++ JZPainter::drawArc 同一套语义。
    bind_p.def("drawArc", [](JZPainter* p, int x, int y, int radius, int startAngle, int endAngle) { p->drawArc(x, y, radius, startAngle, endAngle); });
    bind_p.def("drawText", [](JZPainter* p, int x, int y, const std::string& text) { p->drawText(x, y, text.c_str()); });
    bind_p.def("drawImage", [](JZPainter* p, JZImage* img, int x, int y, int w, int h) {
        if (!img)
            throw std::runtime_error("drawImage: image 不能为空");
        p->drawImage(*img, x, y, w, h);
    });
    // setTransform(matrix: JZMatrix2D) / setTransform(a, b, c, d, e, f) —— 同 setPen 等, 走 C 函数
    bind_p.def("setTransform", js_JZPainter_setTransform, 1);
    /** 当前变换矩阵(拷贝); 读出来改完再 setTransform 回去 */
    bind_p.def("transform", [](JZPainter* p) { return p->transform(); });
    m_engine->registClass(bind_p.classDefine());
}

// 从 createUi 返回的 { root, <id>: widget } 取出 root 窗口, 并把 root 之外的全部属性
// (即按 id 命名的子控件)挂到 root 上, 使 ui 既是 JZWindow, 又能通过 ui.<id> 访问子控件。
static JSValue uiLoaderResultToWindow(JSContext* ctx, JSValue uiObj)
{
    JSValue root = JS_GetPropertyStr(ctx, uiObj, "root");
    if (JS_IsUndefined(root) || JS_IsException(root))
    {
        JS_FreeValue(ctx, uiObj);
        return JS_UNDEFINED;
    }
    JSPropertyEnum* props = nullptr;
    uint32_t len = 0;
    JS_GetOwnPropertyNames(ctx, &props, &len, uiObj, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY);
    for (uint32_t i = 0; i < len; i++)
    {
        const char* name = JS_AtomToCString(ctx, props[i].atom);
        if (name && std::strcmp(name, "root") == 0)
        {
            JS_FreeCString(ctx, name);
            continue;
        }
        JSValue v = JS_GetProperty(ctx, uiObj, props[i].atom);
        JS_SetPropertyStr(ctx, root, name, v);   // 该接口**消费** v(quickjs.c: "val is freed by the function"), 不能再 free
        if (name) JS_FreeCString(ctx, name);
    }
    if (props) JS_FreePropertyEnum(ctx, props, len);
    JS_FreeValue(ctx, uiObj);
    return root;
}

// uiLoader.loadFile(path): 解析 UI 文件并直接构造真实控件树, 返回根 JZWindow 窗口。
static JSValue js_uiLoader_loadFile(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "uiLoader.loadFile(file) 需要一个文件路径参数");
    const char* f = JS_ToCString(ctx, argv[0]);
    if (!f) return JS_EXCEPTION;
    std::string path(f);
    JS_FreeCString(ctx, f);

    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    if (!engine)
        return JS_ThrowTypeError(ctx, "uiLoader.loadFile: 无法从 ctx 获取引擎");

    JZJSUiLoader loader;
    if (!loader.loadFromFile(path))
    {
        std::string msg = "uiLoader.loadFile 失败: " + loader.error();
        return JS_ThrowTypeError(ctx, "%s", msg.c_str());
    }
    JSValue uiObj = loader.createUi(engine);
    if (JS_IsUndefined(uiObj))
    {
        std::string msg = "uiLoader.loadFile 构造 UI 失败: " + loader.error();
        return JS_ThrowTypeError(ctx, "%s", msg.c_str());
    }
    return uiLoaderResultToWindow(ctx, uiObj);
}

// uiLoader.load(xmlText): 解析 UI 文本并直接构造真实控件树, 返回根 JZWindow 窗口。
static JSValue js_uiLoader_load(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "uiLoader.load(text) 需要一个 XML 文本参数");
    const char* s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    std::string xml(s);
    JS_FreeCString(ctx, s);

    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    if (!engine)
        return JS_ThrowTypeError(ctx, "uiLoader.load: 无法从 ctx 获取引擎");

    JZJSUiLoader loader;
    if (!loader.load(xml))
    {
        std::string msg = "uiLoader.load 失败: " + loader.error();
        return JS_ThrowTypeError(ctx, "%s", msg.c_str());
    }
    JSValue uiObj = loader.createUi(engine);
    if (JS_IsUndefined(uiObj))
    {
        std::string msg = "uiLoader.load 构造 UI 失败: " + loader.error();
        return JS_ThrowTypeError(ctx, "%s", msg.c_str());
    }
    return uiLoaderResultToWindow(ctx, uiObj);
}

// jzApp.setStyleSheet(css): 设置**全局**样式表(等价 C++ 的 jzApp->setStyleSheet)。
// 换肤 = 整表替换: 变量只有 :root 一处, 且是 parse 期文本替换(见 JZWidget 样式指南),
// 所以切主题要把两份 CSS 各完整给一次, 而不是"改一个变量"。
static JSValue js_jzApp_setStyleSheet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc != 1)
        return JS_ThrowTypeError(ctx, "jzApp.setStyleSheet(css) 需要 1 个参数");

    const char* s = JS_ToCString(ctx, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    std::string css(s);
    JS_FreeCString(ctx, s);

    if (!jzApp)
        return JS_ThrowInternalError(ctx, "jzApp.setStyleSheet: 应用对象不存在(JZApplication 未创建)");
    if (!jzApp->setStyleSheet(css))
        return JS_ThrowSyntaxError(ctx, "样式表语法错误: %s", jzApp->styleSheetError().c_str());
    return JS_TRUE;
}

// ---------- 全局 jzApp ----------
// C++ 侧的进程级应用对象(jzApp, JZApplication)在 JS 侧的入口。与 jzWidget 一样属于"应用级"全局。
//
// 为什么不是 QApplication: 移植代码里的 QApplication/qApp 是**它自己的兼容层**(见 bteserver/src/
// qtcompat.ts), 那一层负责把 Qt 名映射过来; 引擎只提供事实来源 —— 路径与退出。
//   applicationDirPath()  入口脚本所在目录(见 JZJSEngine::appFilePath)
//   applicationFilePath() 入口脚本绝对路径(同上)
//   workingDirPath()      进程当前工作目录
//   quit() / exit(code)   请求事件循环退出(等价 JZApplication::quit/exit)
// 注1: **没加载过脚本(纯 eval 的引擎)时这两个都返回空串** —— 引擎不臆造 exe 那一份;
//      需要"总是有个目录"的兼容层(qtcompat)自己按 workDir -> exe 继续退(见 qtcompat.appDirPath)。
// 注2: 这里**不提供 exec()** —— JS 引擎的事件循环由宿主管(见 qtcompat 的 QApplication::exec)。
namespace
{
// 应用路径的统一算法: 引擎知道入口脚本(多引擎各有各的) -> 没有就是空串。
std::string jzAppPath(JSContext* ctx, bool wantDir)
{
    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    if (!engine || engine->appFilePath().empty())
        return std::string();

    std::string f = engine->appFilePath();
    if (!wantDir)
        return f;

    size_t pos = f.find_last_of("/\\");
    return (pos == std::string::npos) ? std::string() : f.substr(0, pos);
}

JSValue js_jzApp_applicationDirPath(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    return JS_NewString(ctx, jzAppPath(ctx, true).c_str());
}

JSValue js_jzApp_applicationFilePath(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    return JS_NewString(ctx, jzAppPath(ctx, false).c_str());
}

JSValue js_jzApp_workingDirPath(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    return JS_NewString(ctx, JZDir::currentPath().c_str());
}

JSValue js_jzApp_quit(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    if (!jzApp)
        return JS_ThrowInternalError(ctx, "jzApp.quit: 应用对象不存在(JZApplication 未创建)");
    jzApp->quit();
    return JS_UNDEFINED;
}

JSValue js_jzApp_exit(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (!jzApp)
        return JS_ThrowInternalError(ctx, "jzApp.exit: 应用对象不存在(JZApplication 未创建)");

    int code = 0;
    if (argc > 0 && !JS_IsUndefined(argv[0]))
    {
        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        if (!engine || !engine->checkType(argv[0], JSType_int))
            return JS_ThrowTypeError(ctx, "jzApp.exit(code): code 需要数字");
        code = fromJSValue<int>(ctx, argv[0]);
    }

    jzApp->exit(code);
    return JS_UNDEFINED;
}

// JZWindow* jzApp.activeWindow(): 当前活动窗口(优先持键盘焦点的那个; 都没有 -> null)。
// 用途: 弹层/对话框/菜单需要一个 owner 窗口 —— owner 决定弹层的 DPI 缩放与定位基准,
// 而 JS 侧常常只有"当前正在交互的那个窗口"这一个概念(如右键菜单由 jzApp.activeWindow() 定位)。
JSValue js_jzApp_activeWindow(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    if (!jzApp)
        return JS_ThrowInternalError(ctx, "jzApp.activeWindow: 应用对象不存在(JZApplication 未创建)");

    JZWindow* w = jzApp->activeWindow();
    if (!w)
        return JS_NULL;

    // 返回**已有的那份包装**(控件族一个对象只有一份宿主句柄, 现造第二份会把第一份作废) ——
    // 与 JZMainWindow.centralWidget/menuBar 同套路。
    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    JZJSWidgetModule* mod = engine ? engine->widgetModule() : nullptr;
    if (!mod)
        return JS_NULL;

    JSValue v = mod->jsValueOf(w);
    if (!JS_IsUndefined(v))
        return JS_DupValue(ctx, v);

    // 该窗口没有 JS 包装(例如纯 C++ 建的内部窗口): 拿不到视图, 只能报 null。
    return JS_NULL;
}
}   // namespace

void JZJSWidgetModule::registApp()
{
    JSContext* ctx = m_engine->context();

    // JZDesktopServices.openUrl(url) —— 打开链接/文件(等价 Qt QDesktopServices::openUrl)。
    // C++ 侧本来就有这个类, 之前没给 JS 门面; 有界面代码要"点帮助打开网页"就靠它。
    {
        JSValue ds = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, ds, "openUrl", JS_NewCFunction(ctx, [](JSContext* ctx2, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
            if (argc < 1)
                return JS_ThrowTypeError(ctx2, "JZDesktopServices.openUrl(url): 需要 url");
            const std::string url = fromJSValue<std::string>(ctx2, argv[0]);
            return JS_NewBool(ctx2, JZDesktopServices::openUrl(url));
        }, "openUrl", 1));
        m_engine->setProperty(m_engine->globalObject(), "JZDesktopServices", ds);
    }

    JSValue app = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, app, "applicationDirPath",
                      JS_NewCFunction(ctx, js_jzApp_applicationDirPath, "applicationDirPath", 0));
    JS_SetPropertyStr(ctx, app, "applicationFilePath",
                      JS_NewCFunction(ctx, js_jzApp_applicationFilePath, "applicationFilePath", 0));
    JS_SetPropertyStr(ctx, app, "workingDirPath",
                      JS_NewCFunction(ctx, js_jzApp_workingDirPath, "workingDirPath", 0));
    JS_SetPropertyStr(ctx, app, "quit", JS_NewCFunction(ctx, js_jzApp_quit, "quit", 0));
    JS_SetPropertyStr(ctx, app, "exit", JS_NewCFunction(ctx, js_jzApp_exit, "exit", 1));
    JS_SetPropertyStr(ctx, app, "setStyleSheet", JS_NewCFunction(ctx, js_jzApp_setStyleSheet, "setStyleSheet", 1));
    JS_SetPropertyStr(ctx, app, "activeWindow",
                      JS_NewCFunction(ctx, js_jzApp_activeWindow, "activeWindow", 0));
    m_engine->setProperty(m_engine->globalObject(), "jzApp", app);
}

void JZJSWidgetModule::registUiLoader()
{    
    // 全局命名空间 uiLoader: 只暴露 loadFile / load 两个工厂方法, 各自解析 UI 描述并在当前引擎中
    // 直接构造真实控件树, 返回根 JZWindow 窗口(同时把 id 子控件挂为窗口属性, 如 ui.btn)。
    JSContext* ctx = m_engine->context();
    JSValue uiLoader = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, uiLoader, "loadFile", JS_NewCFunction(ctx, js_uiLoader_loadFile, "loadFile", 1));
    JS_SetPropertyStr(ctx, uiLoader, "load", JS_NewCFunction(ctx, js_uiLoader_load, "load", 1));
    m_engine->setProperty(m_engine->globalObject(), "uiLoader", uiLoader);
}