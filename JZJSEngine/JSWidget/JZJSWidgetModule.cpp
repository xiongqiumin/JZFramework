#include "JZJSWidgetModule.h"

#include "JZJSBind.h"
#include "JZJSWidget.h"
#include "JZApplication.h"
#include "JZWidget.h"
#include "JZWindow.h"
#include "JZLayout.h"
#include "JZPainter.h"
#include "JZPen.h"
#include "JZBrush.h"
#include "JZFont.h"
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
#include "JZCheckBox.h"
#include "JZRadioButton.h"
#include "JZSlider.h"
#include "JZProgressBar.h"
#include "JZComboBox.h"
#include "JZSpinBox.h"
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

// 颜色名/十六进制解析 (供 JZPainter 的 setPen/setBrush 与测试复用)
static JZColor jzParseColor(const std::string& s)
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

// 新建 JS 对象回写到 JSWidgetClass<W> 包裹对象(m_jsvalue / m_engine),
// 供事件转发 callJs 以该对象为 this 调用 JS 方法(onPaint / onMousePress ...)。
// 作为 onCreated 钩子, 在 createValue(type, ptr) 内触发 —— 覆盖 new / 返回值转换 / toValuePtr / createCanvas 全部包装路径。
template <class W>
static void jzBindWidgetInstance(JSContext* ctx, JSValue obj, void* opaque, JZJSEngine* engine,
                                 JZJSPaintScope* paintScope, JZJSWidgetModule* module)
{
    // 经多态基类取底层对象: opaque 的包装类型不一定是 JSRawPointer<W>。
    auto* cp = (jsbind::JSPointerBase*)opaque;
    auto* js = static_cast<JSWidgetClass<W>*>((W*)cp->getRaw());
    js->m_engine = engine;
    js->setPaintScope(paintScope);   // paintEvent 里用它界定 JS 侧画笔的生命周期
    js->m_jsvalue = obj;             // 原始句柄(不 dup): 随 JS 自身 GC, 析构不释放, 避免自引用环

    // 构造时就带父控件的(如 new JZWindow(w, h, title, parent)): 底层对象已在父的对象树里,
    // 现在就登记保活 —— 挂进树之后就不该再指望 JS 侧持有包装(见 JZJSWidgetModule.h 的登记说明)。
    // 注意 m_jsvalue 每次造包装都会被覆盖成"最新那份", 所以登记里也持有最新那份(见 bindWidgetToParent)。
    if (module)
    {
        JZWidget* self = js;
        JZWidget* parent = self->parentWidget();
        if (parent)
            module->bindWidgetToParent(parent, self, obj);
    }
}

// ---------- 模块生命周期 ----------

JZJSWidgetModule::JZJSWidgetModule() {}
JZJSWidgetModule::~JZJSWidgetModule() { deinitEngine(); }

void JZJSWidgetModule::initEngine(JZJSEngine* engine)
{
    m_engine = engine;
    m_paintScope.setEngine(engine);   // 画笔作用域随模块: 先绑定引擎, 再注册各类

    // 带 func_data 的 C 函数(setParentWidget)要拿到本模块: 把指针做成一个"整数句柄"备好,
    // 注册时注入(func_data 只在 C 侧可见, JS 侧读不到)。用整数而不是包装对象:
    // 不必为它注册 JS 类/造对象, 也没有所有权与 GC 问题(见 JZJSWidgetModule.h 的 m_widgetData)。
    if (engine)
    {
        m_widgetData = JS_NewInt64(engine->context(), (int64_t)(intptr_t)this);
        engine->setWidgetModule(this);   // 各个 JS 构造函数要拿它登记控件表(见 widgetModule())
    }

    registGeometry();
    registColor();
    registImage();
    registEvents();
    registWidgets();
    registViews();
    registLayouts();
    registPainter();
    registUiLoader();

    // 最后一步: 把各类在全局上的构造函数换成 C 版本(js_JZWidget_ctor 系列) ——
    // 引擎那个只会调 createArgs(收不到实参), 换成 C 版本才能按 JS 实参形态分派,
    // 并在构造后登记控件表(有父就保活)。必须在所有类注册完之后调用。
    jzRegistWidgetJsConstructors(m_engine->context());

    m_typeWindow = m_engine->ctypeidToType(typeid(JZWindow).name());    
}

void JZJSWidgetModule::deinitEngine()
{
    m_paintScope.reset();   // 引擎退出: 回收未结束的画笔并断开引擎引用

    if (m_engine)
    {
        JSContext* ctx = m_engine->context();

        // 登记树里保活的是 JS 包装, 必须在引擎(rt/ctx)还活着的时候放掉。
        // 逐个"摘下来再释放": 释放可能级联销毁控件(进而回调进来摘节点), 不能持有 vector 的引用/迭代器。
        while (!m_widgetList.empty())
        {
            WidgetInfo node = std::move(m_widgetList.back());
            m_widgetList.pop_back();
            freeWidgetInfo(node);
        }

        // 信号回调持有的是 JS 函数值, 同样要在引擎还活着时放掉
        for (JSValue v : m_jsCallbacks)
            JS_FreeValue(ctx, v);
    }
    m_widgetList.clear();
    m_jsCallbacks.clear();

    m_widgetData = JS_UNDEFINED;   // 整数句柄, 无引用计数, 直接丢

    if (m_engine)
        m_engine->setWidgetModule(nullptr);   // 先摘模块, 再断开

    m_engine = nullptr;
    m_typeWindow = -1;    
}

// ---------- 控件登记 (JS 包装保活) ----------
// 见 JZJSWidgetModule.h 顶部说明: 子控件进对象树后, 由父控件的登记节点持一份它的 JS 包装
// (保活), 控件销毁时释放; 顶层控件不保活(生死由 JS 决定)。

JZJSWidgetModule::WidgetInfo* JZJSWidgetModule::findWidgetInfoIn(std::vector<WidgetInfo>& list, JZObject* object)
{
    for (int i = 0; i < (int)list.size(); i++)
    {
        if (list[i].object == object)
            return &list[i];
        WidgetInfo* child = findWidgetInfoIn(list[i].children, object);
        if (child)
            return child;
    }
    return nullptr;
}

JZJSWidgetModule::WidgetInfo* JZJSWidgetModule::findWidgetInfo(JZObject* object)
{
    if (!object)
        return nullptr;
    return findWidgetInfoIn(m_widgetList, object);
}

bool JZJSWidgetModule::takeWidgetInfoIn(std::vector<WidgetInfo>& list, JZObject* object, WidgetInfo& out)
{
    for (int i = 0; i < (int)list.size(); i++)
    {
        if (list[i].object == object)
        {
            // move 出来再 erase: 子树与保活引用一起搬走, 调用方决定挂到哪/是否释放
            out = std::move(list[i]);
            list.erase(list.begin() + i);
            return true;
        }
        if (takeWidgetInfoIn(list[i].children, object, out))
            return true;
    }
    return false;
}

bool JZJSWidgetModule::takeWidgetInfo(JZObject* object, WidgetInfo& out)
{
    if (!object)
        return false;
    return takeWidgetInfoIn(m_widgetList, object, out);
}

JZJSWidgetModule::WidgetInfo* JZJSWidgetModule::pushWidgetInfo(std::vector<WidgetInfo>& list, JZObject* object)
{
    if (!object)
        return nullptr;

    list.push_back(WidgetInfo());
    WidgetInfo* node = &list.back();
    node->object = object;
    connect(&object->sigDestroyed, this, &JZJSWidgetModule::onWidgetDestroyed);
    return node;
}

JZJSWidgetModule::WidgetInfo* JZJSWidgetModule::ensureWidgetInfo(JZObject* object)
{
    if (!object)
        return nullptr;

    WidgetInfo* node = findWidgetInfo(object);
    if (node)
        return node;

    // 有父控件: 挂到父节点下(父节点没有就递归补建 —— 父链上一般只有窗口是"没登记过"的);
    // 没有父控件: 作为顶层节点, jsvalue 恒为空 = 不保活。
    JZWidget* widget = jzobject_cast<JZWidget*>(object);
    JZWidget* parent = widget ? widget->parentWidget() : nullptr;
    if (parent)
    {
        WidgetInfo* pnode = ensureWidgetInfo(parent);
        if (pnode)
            return pushWidgetInfo(pnode->children, object);
    }

    return pushWidgetInfo(m_widgetList, object);
}

void JZJSWidgetModule::freeWidgetInfo(WidgetInfo& node)
{
    // 释放的是**模块自己那份**引用: 控件已被对象树 delete(或交回 JS)时才走到这里,
    // 包装的守卫指针已置空, 所以不会二次删除。释放可能级联触发 JS 侧的收尾(进而删控件、
    // 回调进来摘别的节点), 所以: ① node 必须是"摘下来的副本", 不是树里正在遍历的元素;
    // ② 先把引用清空再释放, 免得级联路径重入到此节点时二次释放。
    if (m_engine && !JS_IsUndefined(node.jsvalue))
    {
        JSValue held = node.jsvalue;
        node.jsvalue = JS_UNDEFINED;
        JS_FreeValue(m_engine->context(), held);
    }
    node.jsvalue = JS_UNDEFINED;

    for (int i = 0; i < (int)node.children.size(); i++)
        freeWidgetInfo(node.children[i]);
    node.children.clear();
    node.object = nullptr;
}

void JZJSWidgetModule::removeWidgetInfo(JZObject* object)
{
    WidgetInfo node;
    if (!takeWidgetInfo(object, node))
        return;
    freeWidgetInfo(node);
}

void JZJSWidgetModule::onWidgetDestroyed(JZObject *object)
{
    // 控件正在析构(~JZObject 末尾, 守卫指针已置空, 子控件也已 delete): 摘掉它的节点与子树,
    // 放掉保活引用。这里只按指针比较, 绝不访问对象本身。
    removeWidgetInfo(object);
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

void JZJSWidgetModule::bindWidgetToParent(JZWidget* parent, JZWidget* child, JSValueConst childValue)
{
    if (!m_engine || !parent || !child || parent == child)
        return;

    // 已经登记过(改父 / 重复 addWidget): 摘下来(子树与保活引用一起)搬到新父节点下
    WidgetInfo node;
    bool hadNode = takeWidgetInfo(child, node);

    WidgetInfo* pnode = ensureWidgetInfo(parent);
    if (!pnode)
        return;

    // 新节点由 pushWidgetInfo 建(顺带订阅 sigDestroyed); 已有节点则把保活引用与子树搬过来
    WidgetInfo* dst = pushWidgetInfo(pnode->children, child);
    if (!dst)
        return;
    if (hadNode)
    {
        dst->jsvalue = node.jsvalue;
        dst->children = std::move(node.children);
    }

    // 保活: 子控件的 JS 包装由模块持有一份。否则 JS 侧放手后包装被 GC, 而底层对象还持有
    // 包装句柄(JSWidgetClass::m_jsvalue)做事件转发 -> 往死对象上派发。
    // 持**最新**那份(childValue): m_jsvalue 每次造包装都会被覆盖, 两边指向同一个才不会悬垂。
    if (m_engine && !JS_IsUndefined(dst->jsvalue))
    {
        JSValue old = dst->jsvalue;
        dst->jsvalue = JS_UNDEFINED;   // 先清再放: 级联路径重入不会二次释放
        JS_FreeValue(m_engine->context(), old);
    }
    dst->jsvalue = JS_DupValue(m_engine->context(), childValue);
}

void JZJSWidgetModule::unbindWidget(JZWidget* child)
{
    WidgetInfo node;
    if (!takeWidgetInfo(child, node))
        return;

    // 先把节点挂回顶层 —— 后面释放引用可能让包装立刻析构(进而 delete child 及其子控件),
    // 那些控件的 sigDestroyed 回调要靠"节点在树上"才能被正常摘掉/释放。
    if (node.object)
        m_widgetList.push_back(std::move(node));

    // 交回 JS 拥有: 撤销**本节点**的保活(子树保持登记 —— 那些控件仍由 child 的对象树持有),
    // 本节点变成顶层节点(顶层不保活, 生死由 JS 那份包装决定)。
    // 注意: 释放引用可能 delete child —— 调用方须把本调用放在最后, 之后不得再访问 child。
    if (!m_widgetList.empty())
    {
        WidgetInfo& top = m_widgetList.back();
        if (top.object == child && m_engine && !JS_IsUndefined(top.jsvalue))
        {
            JSValue held = top.jsvalue;
            top.jsvalue = JS_UNDEFINED;   // 先清引用再释放: 级联路径若重入不会二次释放
            JS_FreeValue(m_engine->context(), held);
        }
    }
}

void JZJSWidgetModule::registerCreatedWidget(JZWidget* widget, JSValueConst value)
{
    if (!m_engine || !widget)
        return;

    // 构造时就带父控件的(如 new JZLabel(text, win)): 底层对象已经在父的对象树里, 立刻登记保活;
    // 没有父的(顶层控件): 建一个顶层节点, jsvalue 保持空(不保活 —— 生死由 JS 那份包装决定)。
    JZWidget* parent = widget->parentWidget();
    if (parent)
        bindWidgetToParent(parent, widget, value);
    else
        ensureWidgetInfo(widget);
}

// ---------- 信号 -> JS 回调 ----------

JSValue JZJSWidgetModule::holdJsCallback(JSValue cb)
{
    JSContext* ctx = m_engine->context();
    JSValue held = JS_DupValue(ctx, cb);
    m_jsCallbacks.push_back(held);
    return held;
}

JSValue JZJSWidgetModule::callJsCallback(JSValue fn, int argc, JSValueConst* argv)
{
    JSContext* ctx = m_engine->context();
    // 回调里抛出的异常不能跨 quickjs 的 C 调用栈传回去(信号是 C++ 侧同步 emit 的),
    // 按引擎既有口径: 交给 reportUncaughtError 并入错误链(sigError/sigLog)。
    JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, argc, argv);
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

void JZJSWidgetModule::bindSignal(JZSigVoid& sig, JSValue cb)
{
    JSValue held = holdJsCallback(cb);
    connect(&sig, [this, held]() { callJsCallback(held, 0, nullptr); });
}

void JZJSWidgetModule::bindSignal(JZSigBool& sig, JSValue cb)
{
    JSValue held = holdJsCallback(cb);
    connect(&sig, [this, held](bool v) {
        JSValue a = toJSValue(m_engine->context(), v);
        callJsCallback(held, 1, &a);
    });
}

void JZJSWidgetModule::bindSignal(JZSigInt& sig, JSValue cb)
{
    JSValue held = holdJsCallback(cb);
    connect(&sig, [this, held](int v) {
        JSValue a = toJSValue(m_engine->context(), v);
        callJsCallback(held, 1, &a);
    });
}

void JZJSWidgetModule::bindSignal(JZSigString& sig, JSValue cb)
{
    JSValue held = holdJsCallback(cb);
    connect(&sig, [this, held](const std::string& v) {
        JSValue a = toJSValue(m_engine->context(), v);
        callJsCallback(held, 1, &a);
    });
}

void JZJSWidgetModule::bindSignal(JZSignal<int, int>& sig, JSValue cb)
{
    JSValue held = holdJsCallback(cb);
    connect(&sig, [this, held](int a1, int a2) {
        JSValue args[2] = { toJSValue(m_engine->context(), a1), toJSValue(m_engine->context(), a2) };
        callJsCallback(held, 2, args);
    });
}

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
    m_engine->registClass(bind_point.classDefine());

    jsbind::ClassBind<JZSize> bind_size("JZSize");
    bind_size.def("setWidth", [](JZSize* s, int w) { s->setWidth(w); });
    bind_size.def("setHeight", [](JZSize* s, int h) { s->setHeight(h); });
    bind_size.def("width", [](JZSize* s) { return s->width(); });
    bind_size.def("height", [](JZSize* s) { return s->height(); });
    bind_size.def("isEmpty", [](JZSize* s) { return s->isEmpty(); });
    bind_size.def("isValid", [](JZSize* s) { return s->isValid(); });
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
    m_engine->registClass(bind_img.classDefine());

    defStaticFunction(bind_img.classDefine(), "create", js_JZImageCreate, 2);
}

// ---------- 事件 (按类注册) ----------

// 基类公共方法(type/accept/ignore/isAccepted)按子类分别注册; 用模板而非泛型 lambda,
// 否则把含模板化 operator() 的 lambda 传给 ClassBind::def 时, 绑定 trait strip_function_object
// 无法对 decltype(&F::operator()) 分解, 会编译失败。
template <class E>
static void defEventCommon(jsbind::ClassBind<E>& b)
{
    b.def("type", [](E* e) { return e->type(); });
    b.def("accept", [](E* e) { e->accept(); });
    b.def("ignore", [](E* e) { e->ignore(); });
    b.def("isAccepted", [](E* e) { return e->isAccepted(); });
}

void JZJSWidgetModule::registEvents()
{
    // 基类方法只在 JZEvent 上注册一次, 子类经 JS 原型链继承 (ClassBind<Sub>(name, bind_event))。
    jsbind::ClassBind<JZEvent> bind_event("JZEvent");
    defEventCommon(bind_event);
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

    jsbind::ClassBind<JZResizeEvent> bind_resize("JZResizeEvent");
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
    bind_timer.setCreate([]() { return new jsbind::JSRawPointer<JZTimerEvent>(new JZTimerEvent(0), false); });
    bind_timer.def("id", [](JZTimerEvent* e) { return e->id(); });
    m_engine->registClass(bind_timer.classDefine());
}

// 所有权翻转(/Transfer/ 与 /TransferBack/)用 JZJSWidget.h 里的 jzSetCppOwner:
// 那边一处实现, 这里和 C 函数共用(包装类型随类层次不同, 必须经 JSPointerBase 多态走)。

// 校验"是控件(或其子类)"并取裸指针(不翻转所有权)。出错抛 std::runtime_error
// (由 JZJSEngine::call 的 try/catch 转成 JS 异常)。
static JZWidget* jsWidgetArg(JSContext* ctx, JSValueConst value, const char* what)
{
    if (!js_check_class_arg<JZWidget>(ctx, value))
        throw std::runtime_error(std::string(what) + ": 需要 JZWidget(或其子类)");
    return fromJSValue<JZWidget*>(ctx, value);
}

// 形参是"子控件"的 JSValue: 校验 → 翻成 C++ 拥有(/Transfer/) → 取裸指针。
// 控件一旦挂进对象树就由**父对象**负责 delete, JS 侧必须交回所有权, 否则:
//   ① JS GC 会删掉还挂在树里的控件(父对象手里只剩悬垂指针);
//   ② 退出时 JS 包装与父对象各 delete 一次(实测崩在 operator delete / 堆校验)。
static JZWidget* jsTakeWidgetArg(JSContext* ctx, JSValueConst value, const char* what)
{
    JZWidget* w = jsWidgetArg(ctx, value, what);
    jzSetCppOwner(value, true);
    return w;
}

// 子控件进对象树(带登记): jsTakeWidgetArg 的成员版, 多两步:
//   ① 真的把控件挂到父控件下(JZWidget::setParentWidget) —— 布局的 addWidget 只登记 item,
//      不设父子关系; 既然如此, 上面那次 /Transfer/ 就没有任何一方负责 delete(泄漏),
//      而且"控件销毁 → 撤销保活"这条链也不会触发;
//   ② 登记到父控件的控件表(保活 JS 包装)。
JZWidget* JZJSWidgetModule::takeWidget(JZWidget* parent, JSValueConst value, const char* what)
{
    JZWidget* child = jsWidgetArg(m_engine->context(), value, what);
    jzSetCppOwner(value, true);                  // /Transfer/: 之后由父对象树负责 delete
    if (child->parentWidget() != parent)
        child->setParentWidget(parent);          // 对象树里真的成为子控件
    bindWidgetToParent(parent, child, value);    // 登记 + 保活
    return child;
}

// ---------- 控件与布局 ----------

// JS 侧 new 出来的**控件**统一用守卫指针包装(jsbind::JSObjectPointer):
//   控件都是 JZObject 派生, 挂进对象树后由**父对象**负责 delete; 而 JS 侧包装默认也是
//   "拥有"(cppOwner=false) —— 引擎退出时 GC 终结器与父对象析构会各 delete 一次 = 双重释放。
//   JSObjectPointer 内部的守卫指针在对象析构后自动置空, 第二次 delete 自然不会发生。
//   (JSRawPointer 没有守卫, 只适合"不会被别的所有者删掉"的对象; 见 JZJSType.h 的包装模型。)
// 注: 这只解决"退出/GC 与对象树争着删", 不解决"JS 包装先被 GC, 对象树随后悬垂" ——
//     后者要求 JS 侧持有引用(uiLoader 的 __keep / 演示脚本的 keep 数组都是这个作用)。
template <class W>
static void setWidgetCreate(jsbind::ClassBind<W>& bind)
{
    bind.setCreate([]() -> jsbind::JSPointerBase* {
        return new jsbind::JSObjectPointer(new W(), false);
    });
}

void JZJSWidgetModule::registWidgets()
{
    // 基类 JZWidget —— 实际实例为 JSWidgetClass<JZWidget>, 用于 onPaint 等事件转发
    jsbind::ClassBind<JZWidget> bind_w("JZWidget");
    bind_w.setCreate([this](JSContext* ctx, int, JSValueConst*) -> jsbind::JSPointerBase* {
        return new jsbind::JSObjectPointer(new JSWidgetClass<JZWidget>(), false);   // 守卫指针, 见 setWidgetCreate
    });
    bind_w.setInstanceBind([this](JSContext* ctx, JSValue obj, void* opaque) {
        jzBindWidgetInstance<JZWidget>(ctx, obj, opaque, m_engine, &m_paintScope, this);
    });
    bind_w.def("move", [](JZWidget* w, int x, int y) { w->move(x, y); });
    bind_w.def("x", [](JZWidget* w) { return w->x(); });
    bind_w.def("y", [](JZWidget* w) { return w->y(); });
    bind_w.def("width", [](JZWidget* w) { return w->width(); });
    bind_w.def("height", [](JZWidget* w) { return w->height(); });
    bind_w.def("resize", [](JZWidget* w, int w_, int h) { w->resize(w_, h); });
    bind_w.def("setFixedSize", [](JZWidget* w, int w_, int h) { w->setFixedSize(w_, h); });
    bind_w.def("setFixedWidth", [](JZWidget* w, int v) { w->setFixedWidth(v); });
    bind_w.def("setFixedHeight", [](JZWidget* w, int v) { w->setFixedHeight(v); });
    bind_w.def("setMinimumSize", [](JZWidget* w, int w_, int h) { w->setMinimumSize(w_, h); });
    bind_w.def("setMaximumSize", [](JZWidget* w, int w_, int h) { w->setMaximumSize(w_, h); });
    bind_w.def("setGeometry", [](JZWidget* w, int x, int y, int w_, int h) { w->setGeometry(x, y, w_, h); });
    bind_w.def("geometry", [](JZWidget* w) { return w->geometry(); });
    bind_w.def("setVisible", [](JZWidget* w, bool v) { w->setVisible(v); });
    bind_w.def("isVisible", [](JZWidget* w) { return w->isVisible(); });
    bind_w.def("show", [](JZWidget* w) { w->show(); });
    bind_w.def("hide", [](JZWidget* w) { w->hide(); });
    // setParentWidget / parentWidget / window / mapTo / layout / setLayout: 走**C 函数**而不是 lambda ——
    // 它们要拿 this_val(翻转接收者自己的所有权、无父/无窗口时返回真 null、记布局缓存的失效开关),
    // def 的 lambda 只拿得到"转好的形参"。见 JZJSWidget.h 顶部说明; 第二个参数就是 JS 方法名。
    // setParentWidget 还要往本模块的控件表里登记(把接收者的 JS 包装交给父控件保活), 而 C 函数
    // 拿不到模块 —— 所以它注册成**带 func_data 的 C 函数**: 注入模块指针, 回调第 6 个参数就是它
    // (见 jsbind::ClassBind::defData / JZJSWidgetModule::fromFuncData)。其余几个不需要模块。
    bind_w.defData("setParentWidget", js_JZWidget_setParentWidget, 1, 0, { m_widgetData });
    bind_w.def("parentWidget", js_JZWidget_parentWidget, 0);
    bind_w.def("window", js_JZWidget_window, 0);
    bind_w.def("mapTo", js_JZWidget_mapTo, 2);
    // 布局三件套不动模块: layout() 的包装缓存在 JS 对象自己的隐藏属性上(见 JZJSWidget.cpp),
    // setLayout 只做所有权转移, clearLayout 只让缓存视图失效。
    bind_w.def("layout", js_JZWidget_layout, 0);
    // setLayout: Qt 语义, 只能设一次(已有布局/传 null 都抛错); 换布局先 clearLayout()
    bind_w.def("setLayout", js_JZWidget_setLayout, 1);
    bind_w.def("clearLayout", js_JZWidget_clearLayout, 0);
    bind_w.def("setFocus", [](JZWidget* w) { w->setFocus(); });
    bind_w.def("hasFocus", [](JZWidget* w) { return w->hasFocus(); });
    bind_w.def("update", [](JZWidget* w) { w->update(); });
    bind_w.def("setTooltip", [](JZWidget* w, const std::string& s) { w->setTooltip(JZString(s.c_str())); });
    bind_w.def("tooltip", [](JZWidget* w) { return w->tooltip().toStdString(); });
    bind_w.def("setStyleSheet", [](JZWidget* w, const std::string& css) { return w->setStyleSheet(css); });
    bind_w.def("styleSheet", [](JZWidget* w) { return w->styleSheet(); });
    // 对象名(JZObject): 样式表 #id 选择器、AI 定位、自检都靠它
    bind_w.def("setObjectName", [](JZWidget* w, const std::string& n) { w->setObjectName(n); });
    bind_w.def("objectName", [](JZWidget* w) { return w->objectName(); });
    // 样式类名(空格分隔, 供 .class 选择器)
    bind_w.def("setStyleClass", [](JZWidget* w, const std::string& cls) { w->setStyleClass(cls); });
    bind_w.def("styleClass", [](JZWidget* w) { return w->styleClass(); });
    // 尺寸上下限的单项设置(布局里给"固定/伸缩"留余地, 比 setMinimumSize 好用)
    bind_w.def("setMinimumWidth", [](JZWidget* w, int v) { w->setMinimumWidth(v); });
    bind_w.def("setMinimumHeight", [](JZWidget* w, int v) { w->setMinimumHeight(v); });
    bind_w.def("setMaximumWidth", [](JZWidget* w, int v) { w->setMaximumWidth(v); });
    bind_w.def("setMaximumHeight", [](JZWidget* w, int v) { w->setMaximumHeight(v); });
    // 自身矩形(控件本地坐标: x/y 为 0), 绘制回调里填背景用
    bind_w.def("rect", [](JZWidget* w) { return w->rect(); });
    m_engine->registClass(bind_w.classDefine());

    // JZWindow (继承 JZWidget) —— 实际实例为 JSWidgetClass<JZWindow>
    // 注意: 这里**不再**预装默认 VBox —— 布局是"只能设一次"的(Qt 语义, 见 JZWidget::setLayout),
    // 预装会把 JS 里很常见的 `new JZWindow(...); win.setLayout(v)` 变成"重复设置"而被拒。
    // JS 想要默认纵向布局就直接 win.addWidget(...)(JZWindow.addWidget 会按需补一个 VBox)。
    auto makeWindow = [](int w, int h, const std::string& title, JZWidget* parent) {
        return new JSWidgetClass<JZWindow>(w, h, title, parent);
    };
    jsbind::ClassBind<JZWindow> bind_win("JZWindow", bind_w);
    bind_win.setCreate([this, makeWindow](JSContext* ctx, int argc, JSValueConst* argv) -> jsbind::JSPointerBase* {
        int w = 800, h = 600;
        std::string title = "JZWindow";
        JZWidget* parent = nullptr;
        if (argc >= 1) w = fromJSValue<int>(ctx, argv[0]);
        if (argc >= 2) h = fromJSValue<int>(ctx, argv[1]);
        if (argc >= 3) title = fromJSValue<std::string>(ctx, argv[2]);
        if (argc >= 4)
        {
            // 构造参数没有参数类型表(call() 的形参校验帮不上), 所以在转换前先校验。
            // createArgs 约定: 出错"设 JS 异常 + 返回 nullptr"(不抛 C++ 异常)。
            if (!js_check_class_arg<JZWidget>(ctx, argv[3]))
            {
                JS_ThrowTypeError(ctx, "new JZWindow(w, h, title, parent): parent 需要 JZWidget");
                return nullptr;
            }
            parent = fromJSValue<JZWidget*>(ctx, argv[3]);
        }
        return new jsbind::JSObjectPointer(makeWindow(w, h, title, parent), false);   // 守卫指针, 见 setWidgetCreate
    });
    bind_win.setInstanceBind([this](JSContext* ctx, JSValue obj, void* opaque) {
        jzBindWidgetInstance<JZWindow>(ctx, obj, opaque, m_engine, &m_paintScope, this);
    });
    bind_win.def("setTitle", [](JZWindow* w, const std::string& t) { w->setTitle(t); });
    bind_win.def("title", [](JZWindow* w) { return w->title(); });
    bind_win.def("show", [](JZWindow* w) { w->show(); });
    bind_win.def("close", [](JZWindow* w) { w->close(); });
    bind_win.def("addWidget", [this](JZWindow* w, JSValue c) {
        // 父控件就是本窗口: 登记在窗口节点下(窗口自己是顶层节点, 不保活; 子控件保活)
        JZWidget* child = takeWidget(w, c, "JZWindow.addWidget");
        JZLayout* lay = w->layout();
        if (!lay) { lay = new JZVBoxLayout(); w->setLayout(lay); }
        static_cast<JZBoxLayout*>(lay)->addWidget(child, 0);
    });
    m_engine->registClass(bind_win.classDefine());    

    // JZLabel
    jsbind::ClassBind<JZLabel> bind_label("JZLabel", bind_w);
    bind_label.def("setText", &JZLabel::setText);
    bind_label.def("text", &JZLabel::text);
    bind_label.def("setAlign", [](JZLabel* w, int a) { w->setAlign(a); });
    setWidgetCreate(bind_label);
    m_engine->registClass(bind_label.classDefine());

    // JZPushButton
    jsbind::ClassBind<JZPushButton> bind_btn("JZPushButton", bind_w);
    bind_btn.def("setText", &JZPushButton::setText);
    bind_btn.def("text", &JZPushButton::text);
    bind_btn.def("setIcon", [](JZPushButton* w, JZImage* img) { w->setIcon(*img); });
    bind_btn.def("icon", [](JZPushButton* w) { return w->icon(); });
    bind_btn.def("setWidth", [](JZPushButton* w, int v) { w->setWidth(v); });
    bind_btn.def("setHeight", [](JZPushButton* w, int v) { w->setHeight(v); });
    // 点击信号: 把 sigClicked 转接到 JS 回调 (回调在引擎上下文内同步调用)
    bind_btn.def("onClicked", [this](JZPushButton* w, JSValue cb) { bindSignal(w->sigClicked, cb); });
    setWidgetCreate(bind_btn);
    m_engine->registClass(bind_btn.classDefine());

    // JZLineEdit (文本编辑接口在 JZAbstractTextEdit)
    // 注意: setText/text 在 JZAbstractTextEdit 上, 这里必须写 lambda 而不是成员指针 ——
    // `&JZLineEdit::setText` 推导出的宿主类是 JZAbstractTextEdit, 而那个类**没有注册到 JS**,
    // 它的形参类型解析成 JSType_none ⇒ 调用时"参数0类型不匹配"直接抛异常。
    jsbind::ClassBind<JZLineEdit> bind_edit("JZLineEdit", bind_w);
    bind_edit.def("setText", [](JZLineEdit* w, const std::string& s) { w->setText(s); });
    bind_edit.def("text", [](JZLineEdit* w) { return w->text(); });
    bind_edit.def("setPassword", [](JZLineEdit* w, bool on) { w->setPassword(on); });
    bind_edit.def("isPassword", [](JZLineEdit* w) { return w->isPassword(); });
    bind_edit.def("setMaxLength", [](JZLineEdit* w, int n) { w->setMaxLength(n); });
    bind_edit.def("maxLength", [](JZLineEdit* w) { return w->maxLength(); });
    bind_edit.def("setReadOnly", [](JZLineEdit* w, bool on) { w->setReadOnly(on); });
    bind_edit.def("isReadOnly", [](JZLineEdit* w) { return w->isReadOnly(); });
    bind_edit.def("setPlaceholderText", [](JZLineEdit* w, const std::string& s) { w->setPlaceholderText(s); });
    bind_edit.def("placeholderText", [](JZLineEdit* w) { return w->placeholderText(); });
    // 文本类信号(实现在 JZAbstractTextEdit 上)
    bind_edit.def("onTextChanged", [this](JZLineEdit* w, JSValue cb) { bindSignal(w->sigTextChanged, cb); });
    bind_edit.def("onReturnPressed", [this](JZLineEdit* w, JSValue cb) { bindSignal(w->sigReturnPressed, cb); });
    bind_edit.def("onEditingFinished", [this](JZLineEdit* w, JSValue cb) { bindSignal(w->sigEditingFinished, cb); });
    setWidgetCreate(bind_edit);
    m_engine->registClass(bind_edit.classDefine());

    // JZCheckBox
    jsbind::ClassBind<JZCheckbox> bind_check("JZCheckbox", bind_w);
    bind_check.def("setText", &JZCheckbox::setText);
    bind_check.def("text", &JZCheckbox::text);
    bind_check.def("setChecked", [](JZCheckbox* w, bool on) { w->setChecked(on); });
    bind_check.def("isChecked", [](JZCheckbox* w) { return w->isChecked(); });
    bind_check.def("onToggled", [this](JZCheckbox* w, JSValue cb) { bindSignal(w->sigToggled, cb); });
    setWidgetCreate(bind_check);
    m_engine->registClass(bind_check.classDefine());

    // JZRadioButton
    jsbind::ClassBind<JZRadioButton> bind_radio("JZRadioButton", bind_w);
    bind_radio.def("setText", &JZRadioButton::setText);
    bind_radio.def("text", &JZRadioButton::text);
    bind_radio.def("setChecked", [](JZRadioButton* w, bool on) { w->setChecked(on); });
    bind_radio.def("isChecked", [](JZRadioButton* w) { return w->isChecked(); });
    bind_radio.def("onToggled", [this](JZRadioButton* w, JSValue cb) { bindSignal(w->sigToggled, cb); });
    setWidgetCreate(bind_radio);
    m_engine->registClass(bind_radio.classDefine());

    // JZSlider
    jsbind::ClassBind<JZSlider> bind_slider("JZSlider", bind_w);
    bind_slider.def("setRange", [](JZSlider* w, int min, int max) { w->setRange(min, max); });
    bind_slider.def("setValue", [](JZSlider* w, int v) { w->setValue(v); });
    bind_slider.def("value", [](JZSlider* w) { return w->value(); });
    bind_slider.def("onValueChanged", [this](JZSlider* w, JSValue cb) { bindSignal(w->sigValueChanged, cb); });
    setWidgetCreate(bind_slider);
    m_engine->registClass(bind_slider.classDefine());

    // JZProgressBar
    jsbind::ClassBind<JZProgressBar> bind_bar("JZProgressBar", bind_w);
    bind_bar.def("setRange", [](JZProgressBar* w, int min, int max) { w->setRange(min, max); });
    bind_bar.def("setValue", [](JZProgressBar* w, int v) { w->setValue(v); });
    bind_bar.def("value", [](JZProgressBar* w) { return w->value(); });
    setWidgetCreate(bind_bar);
    m_engine->registClass(bind_bar.classDefine());

    // JZComboBox
    jsbind::ClassBind<JZComboBox> bind_combo("JZComboBox", bind_w);
    bind_combo.def("addItem", [](JZComboBox* w, const std::string& s) { w->addItem(s); });
    // 形参用 JSValue 不用 vector<string>: JS 数组过不了引擎的形参类型校验(数组 class id 不在
    // 类型表里, 见 JZChart 那段注释), 所以自行转换。
    bind_combo.def("addItems", [this](JZComboBox* w, JSValue items) {
        w->addItems(jzJsToStringArray(m_engine->context(), items, "JZComboBox.addItems"));
    });
    bind_combo.def("removeItem", [](JZComboBox* w, int i) { w->removeItem(i); });
    bind_combo.def("clear", [](JZComboBox* w) { w->clear(); });
    bind_combo.def("currentIndex", [](JZComboBox* w) { return w->currentIndex(); });
    bind_combo.def("setCurrentIndex", [](JZComboBox* w, int i) { w->setCurrentIndex(i); });
    bind_combo.def("currentText", [](JZComboBox* w) { return w->currentText(); });
    bind_combo.def("setCurrentText", [](JZComboBox* w, const std::string& s) { w->setCurrentText(s); });
    bind_combo.def("itemText", [](JZComboBox* w, int i) { return w->itemText(i); });
    bind_combo.def("count", [](JZComboBox* w) { return w->count(); });
    bind_combo.def("onCurrentIndexChanged", [this](JZComboBox* w, JSValue cb) { bindSignal(w->currentIndexChanged, cb); });
    bind_combo.def("onCurrentTextChanged", [this](JZComboBox* w, JSValue cb) { bindSignal(w->currentTextChanged, cb); });
    setWidgetCreate(bind_combo);
    m_engine->registClass(bind_combo.classDefine());

    // JZSpinBox
    jsbind::ClassBind<JZSpinBox> bind_spin("JZSpinBox", bind_w);
    bind_spin.def("setRange", [](JZSpinBox* w, int min, int max) { w->setRange(min, max); });
    bind_spin.def("setMinimum", [](JZSpinBox* w, int v) { w->setMinimum(v); });
    bind_spin.def("minimum", [](JZSpinBox* w) { return w->minimum(); });
    bind_spin.def("setMaximum", [](JZSpinBox* w, int v) { w->setMaximum(v); });
    bind_spin.def("maximum", [](JZSpinBox* w) { return w->maximum(); });
    bind_spin.def("setStep", [](JZSpinBox* w, int v) { w->setStep(v); });
    bind_spin.def("step", [](JZSpinBox* w) { return w->step(); });
    bind_spin.def("setValue", [](JZSpinBox* w, int v) { w->setValue(v); });
    bind_spin.def("value", [](JZSpinBox* w) { return w->value(); });
    bind_spin.def("setSuffix", [](JZSpinBox* w, const std::string& s) { w->setSuffix(s); });
    bind_spin.def("suffix", [](JZSpinBox* w) { return w->suffix(); });
    bind_spin.def("setReadOnly", [](JZSpinBox* w, bool on) { w->setReadOnly(on); });
    bind_spin.def("isReadOnly", [](JZSpinBox* w) { return w->isReadOnly(); });
    bind_spin.def("onValueChanged", [this](JZSpinBox* w, JSValue cb) { bindSignal(w->sigValueChanged, cb); });
    setWidgetCreate(bind_spin);
    m_engine->registClass(bind_spin.classDefine());

    // JZGroupBox
    jsbind::ClassBind<JZGroupBox> bind_group("JZGroupBox", bind_w);
    bind_group.def("setTitle", [](JZGroupBox* w, const std::string& s) { w->setTitle(s); });
    bind_group.def("title", [](JZGroupBox* w) { return w->title(); });
    bind_group.def("contentWidget", [](JZGroupBox* w) { return w->contentWidget(); }, jsbind::returnOwnedByCpp);
    bind_group.def("setLayout", [this](JZGroupBox* w, JSValue layout) {
        JSContext* ctx = m_engine->context();
        if (!js_check_class_arg<JZLayout>(ctx, layout))
            throw std::runtime_error("JZGroupBox.setLayout: 需要 JZLayout(或其子类)");
        jzSetCppOwner(layout, true);   // 同 JZWidget.setLayout: 控件接管布局
        w->setLayout(fromJSValue<JZLayout*>(ctx, layout));
    });
    setWidgetCreate(bind_group);
    m_engine->registClass(bind_group.classDefine());

    // JZSwitch (开关: on/off)
    jsbind::ClassBind<JZSwitch> bind_switch("JZSwitch", bind_w);
    bind_switch.def("setState", [](JZSwitch* w, bool on) { w->setState(on); });
    bind_switch.def("isOn", [](JZSwitch* w) { return w->isOn(); });
    bind_switch.def("onToggled", [this](JZSwitch* w, JSValue cb) { bindSignal(w->sigToggled, cb); });
    setWidgetCreate(bind_switch);
    m_engine->registClass(bind_switch.classDefine());

    // JZTextEdit (多行文本编辑; 只读时可直接当滚动日志用)
    // 同 JZLineEdit: 文本接口都在 JZAbstractTextEdit(未注册) 上, 全部写 lambda 包一层。
    jsbind::ClassBind<JZTextEdit> bind_text("JZTextEdit", bind_w);
    bind_text.def("setText", [](JZTextEdit* w, const std::string& s) { w->setText(s); });
    bind_text.def("text", [](JZTextEdit* w) { return w->text(); });
    bind_text.def("setPlaceholderText", [](JZTextEdit* w, const std::string& s) { w->setPlaceholderText(s); });
    bind_text.def("placeholderText", [](JZTextEdit* w) { return w->placeholderText(); });
    bind_text.def("setReadOnly", [](JZTextEdit* w, bool on) { w->setReadOnly(on); });
    bind_text.def("isReadOnly", [](JZTextEdit* w) { return w->isReadOnly(); });
    bind_text.def("setLineWrap", [](JZTextEdit* w, bool wrap) { w->setLineWrap(wrap); });
    bind_text.def("lineWrap", [](JZTextEdit* w) { return w->lineWrap(); });
    bind_text.def("cursorPosition", [](JZTextEdit* w) { return w->cursorPosition(); });
    bind_text.def("setCursorPosition", [](JZTextEdit* w, int pos) { w->setCursorPosition(pos); });
    bind_text.def("selectAll", [](JZTextEdit* w) { w->selectAll(); });
    bind_text.def("onTextChanged", [this](JZTextEdit* w, JSValue cb) { bindSignal(w->sigTextChanged, cb); });
    setWidgetCreate(bind_text);
    m_engine->registClass(bind_text.classDefine());

    // JZChart (图表: 折线 / 柱状 / 饼图)
    // 与 JZLabel 等自绘控件同样用默认构造: JZChart 的数据模型是值语义的(序列由控件内部的
    // vector 持有, 没有需要 JS 侧持有/失效的指针), 故不需要 JZJS<Native> 镜像类的那套所有权处理。
    // 唯一要额外做的是序列读取的**越界校验** —— 基类 series(i) 是 vector[i] 不做检查, JS 传错下标
    // 不能把进程带走(抛异常, 由 call() 转成 JS 异常)。
    jsbind::ClassBind<JZChart> bind_chart("JZChart", bind_w);
    bind_chart.def("setChartType", [](JZChart* c, int type) {
        if (type < (int)JZChart::LineChart || type > (int)JZChart::PieChart)
            throw std::runtime_error("JZChart.setChartType: 需要 JZChart.LineChart/BarChart/PieChart");
        c->setChartType((JZChart::ChartType)type);
    });
    bind_chart.def("chartType", [](JZChart* c) { return (int)c->chartType(); });
    // 颜色参数用 CSS 风格字符串('#rrggbb' / 'red'), 与 JZPainter.setPen/setBrush 一致;
    // 传空串取默认调色板(与 setData 的自动配色一致)。
    bind_chart.def("addSeries", [](JZChart* c, const std::string& name, const std::string& color) {
        JZColor col = color.empty() ? JZChart::paletteColor(c->seriesCount()) : jzParseColor(color);
        return c->addSeries(name, col);
    });
    bind_chart.def("setSeriesData", [this](JZChart* c, int index, JSValue data) {
        c->setSeriesData(index, jzJsToDoubleArray(m_engine->context(), data, "JZChart.setSeriesData"));
    });
    bind_chart.def("setSeriesName", [](JZChart* c, int index, const std::string& name) {
        c->setSeriesName(index, name);
    });
    bind_chart.def("setSeriesColor", [](JZChart* c, int index, const std::string& color) {
        c->setSeriesColor(index, jzParseColor(color));
    });
    bind_chart.def("clearSeries", [](JZChart* c) { c->clearSeries(); });
    bind_chart.def("seriesCount", [](JZChart* c) { return c->seriesCount(); });
    bind_chart.def("seriesName", [](JZChart* c, int index) -> std::string {
        if (index < 0 || index >= c->seriesCount())
            throw std::runtime_error("JZChart.seriesName: 序列下标越界");
        return c->series(index).name;
    });
    bind_chart.def("seriesColor", [](JZChart* c, int index) -> JZColor {
        if (index < 0 || index >= c->seriesCount())
            throw std::runtime_error("JZChart.seriesColor: 序列下标越界");
        return c->series(index).color;
    });
    bind_chart.def("seriesData", [](JZChart* c, int index) -> std::vector<double> {
        if (index < 0 || index >= c->seriesCount())
            throw std::runtime_error("JZChart.seriesData: 序列下标越界");
        return c->series(index).data;
    });
    // 一次性设置多条序列: setData([[1,2],[3,4]]) —— 颜色/名称用默认值(独立配色, 名称 "Series N")
    bind_chart.def("setData", [this](JZChart* c, JSValue data) {
        c->setData(jzJsToDoubleMatrix(m_engine->context(), data, "JZChart.setData"));
    });
    // 分类标签: 折线/柱状的 X 轴标签, 饼图的扇区名称
    bind_chart.def("setCategories", [this](JZChart* c, JSValue categories) {
        c->setCategories(jzJsToStringArray(m_engine->context(), categories, "JZChart.setCategories"));
    });
    bind_chart.def("categories", [](JZChart* c) { return c->categories(); });   // 值语义: 返回拷贝
    bind_chart.def("setTitle", [](JZChart* c, const std::string& title) { c->setTitle(title); });
    bind_chart.def("title", [](JZChart* c) { return c->title(); });
    bind_chart.def("setShowGrid", [](JZChart* c, bool on) { c->setShowGrid(on); });
    bind_chart.def("setShowLegend", [](JZChart* c, bool on) { c->setShowLegend(on); });
    bind_chart.def("setShowAxis", [](JZChart* c, bool on) { c->setShowAxis(on); });
    bind_chart.def("setShowPoint", [](JZChart* c, bool on) { c->setShowPoint(on); });
    bind_chart.def("setShowValue", [](JZChart* c, bool on) { c->setShowValue(on); });
    setWidgetCreate(bind_chart);
    m_engine->registClass(bind_chart.classDefine());

    // 类型常量挂到构造器上, JS 侧写 JZChart.BarChart(与 C++ 枚举同值)。
    // 注: classConstructor 返回新引用; JS_SetPropertyStr 会**消费** value(不能再 free)。
    {
        int chart_type_id = m_engine->ctypeidToType(typeid(JZChart).name());
        if (chart_type_id != JSType_none)
        {
            JSContext* ctx = m_engine->context();
            JSValue js_class = m_engine->classConstructor(chart_type_id);
            JS_SetPropertyStr(ctx, js_class, "LineChart", JS_NewInt32(ctx, (int)JZChart::LineChart));
            JS_SetPropertyStr(ctx, js_class, "BarChart", JS_NewInt32(ctx, (int)JZChart::BarChart));
            JS_SetPropertyStr(ctx, js_class, "PieChart", JS_NewInt32(ctx, (int)JZChart::PieChart));
            JS_FreeValue(ctx, js_class);
        }
    }

}

// ---------- 容器 / 视图类控件 ----------
//
// 这些控件的数据/子项由 C++ 持有, JS 侧只做"值级"操作(文本、行数、当前项、滚动位置),
// **不从 JS 创建 item 对象** —— 因此不需要 JZJSListWidget/JZJSTableWidget 那套镜像类
// (它们解决的是"JS new 出来的 item 交给容器"的所有权/失效, 待后续接线)。
void JZJSWidgetModule::registViews()
{
    // 基类绑定: 只为拿到 superCTypeid(ctypeid 即 typeid(JZWidget), 与 registWidgets 里那份一致);
    // JZWidget 本身已经在 registWidgets 注册过, 这里不重复注册。
    jsbind::ClassBind<JZWidget> bind_w("JZWidget");

    // JZListWidget —— JS 侧选页/列表用
    jsbind::ClassBind<JZListWidget> bind_list("JZListWidget", bind_w);
    bind_list.def("appendItem", [](JZListWidget* l, const std::string& t) { l->appendItem(t); });
    bind_list.def("appendItems", [this](JZListWidget* l, JSValue texts) {
        l->appendItems(jzJsToStringArray(m_engine->context(), texts, "JZListWidget.appendItems"));
    });
    bind_list.def("insertItem", [](JZListWidget* l, int i, const std::string& t) { l->insertItem(i, t); });
    // JZListWidget 与 Qt 一样**没有** removeItem: 摘除 + 释放 = takeItem 后自己 delete
    bind_list.def("removeItem", [](JZListWidget* l, int i) { delete l->takeItem(i); });
    bind_list.def("clear", [](JZListWidget* l) { l->clear(); });
    bind_list.def("count", [](JZListWidget* l) { return l->count(); });
    bind_list.def("itemText", [](JZListWidget* l, int i) { return l->itemText(i); });
    bind_list.def("currentIndex", [](JZListWidget* l) { return l->currentIndex(); });
    bind_list.def("setCurrentIndex", [](JZListWidget* l, int i) { l->setCurrentIndex(i); });
    bind_list.def("setItemHeight", [](JZListWidget* l, int h) { l->setItemHeight(h); });
    bind_list.def("onCurrentIndexChanged", [this](JZListWidget* l, JSValue cb) { bindSignal(l->currentIndexChanged, cb); });
    bind_list.def("onActivated", [this](JZListWidget* l, JSValue cb) { bindSignal(l->sigActivated, cb); });
    setWidgetCreate(bind_list);
    m_engine->registClass(bind_list.classDefine());

    // JZTableWidget —— 值级单元格接口(setItemText/itemText 会自动建/取单元格)
    jsbind::ClassBind<JZTableWidget> bind_table("JZTableWidget", bind_w);
    bind_table.def("setRowCount", [](JZTableWidget* t, int n) { t->setRowCount(n); });
    bind_table.def("rowCount", [](JZTableWidget* t) { return t->rowCount(); });
    bind_table.def("setColumnCount", [](JZTableWidget* t, int n) { t->setColumnCount(n); });
    bind_table.def("columnCount", [](JZTableWidget* t) { return t->columnCount(); });
    bind_table.def("setItemText", [](JZTableWidget* t, int r, int c, const std::string& s) { t->setItemText(r, c, s); });
    bind_table.def("itemText", [](JZTableWidget* t, int r, int c) { return t->itemText(r, c); });
    bind_table.def("insertRow", [](JZTableWidget* t, int r) { t->insertRow(r); });
    bind_table.def("removeRow", [](JZTableWidget* t, int r) { t->removeRow(r); });
    bind_table.def("insertColumn", [](JZTableWidget* t, int c) { t->insertColumn(c); });
    bind_table.def("removeColumn", [](JZTableWidget* t, int c) { t->removeColumn(c); });
    bind_table.def("clearContents", [](JZTableWidget* t) { t->clearContents(); });
    bind_table.def("setColumnWidth", [](JZTableWidget* t, int c, int w) { t->setColumnWidth(c, w); });
    bind_table.def("setRowHeight", [](JZTableWidget* t, int h) { t->setRowHeight(h); });
    bind_table.def("rowHeight", [](JZTableWidget* t) { return t->rowHeight(); });
    bind_table.def("setHorizontalHeaderLabels", [this](JZTableWidget* t, JSValue labels) {
        t->setHorizontalHeaderLabels(jzJsToStringArray(m_engine->context(), labels, "JZTableWidget.setHorizontalHeaderLabels"));
    });
    bind_table.def("setCurrentCell", [](JZTableWidget* t, int r, int c) { t->setCurrentCell(r, c); });
    bind_table.def("currentRow", [](JZTableWidget* t) { return t->currentRow(); });
    bind_table.def("currentColumn", [](JZTableWidget* t) { return t->currentColumn(); });
    bind_table.def("onCurrentRowChanged", [this](JZTableWidget* t, JSValue cb) { bindSignal(t->currentRowChanged, cb); });
    setWidgetCreate(bind_table);
    m_engine->registClass(bind_table.classDefine());

    // JZStackedWidget —— 页容器(注意: 构造函数**没有**默认实参, 必须自定义工厂)
    jsbind::ClassBind<JZStackedWidget> bind_stack("JZStackedWidget", bind_w);
    bind_stack.setCreate([](JSContext*, int, JSValueConst*) -> jsbind::JSPointerBase* {
        // 构造函数没有默认实参 ⇒ 不能走 setWidgetCreate 的模板
        return new jsbind::JSObjectPointer(new JZStackedWidget(nullptr), false);
    });
    bind_stack.def("addWidget", [this](JZStackedWidget* s, JSValue w) {
        // 父控件 = 本容器: 走 takeWidget(所有权转移 + 登记保活), 与 JZWindow.addWidget 同一套
        return s->addWidget(takeWidget(s, w, "JZStackedWidget.addWidget"));
    });
    bind_stack.def("count", [](JZStackedWidget* s) { return s->count(); });
    bind_stack.def("currentIndex", [](JZStackedWidget* s) { return s->currentIndex(); });
    bind_stack.def("setCurrentIndex", [](JZStackedWidget* s, int i) { s->setCurrentIndex(i); });
    bind_stack.def("currentWidget", [](JZStackedWidget* s) { return s->currentWidget(); }, jsbind::returnOwnedByCpp);
    m_engine->registClass(bind_stack.classDefine());

    // JZTabWidget —— 标签页(页面所有权: 加入后由控件持有)
    jsbind::ClassBind<JZTabWidget> bind_tab("JZTabWidget", bind_w);
    // 页面所有权: 加入后由标签控件持有 ⇒ 形参收 JSValue 把 JS 侧包装翻成 C++ 拥有(/Transfer/)
    // 并登记保活(父控件 = 本标签控件)
    bind_tab.def("addTab", [this](JZTabWidget* t, JSValue page, const std::string& label) {
        return t->addTab(takeWidget(t, page, "JZTabWidget.addTab"), label);
    });
    bind_tab.def("count", [](JZTabWidget* t) { return t->count(); });
    bind_tab.def("tabText", [](JZTabWidget* t, int i) { return t->tabText(i); });
    bind_tab.def("currentIndex", [](JZTabWidget* t) { return t->currentIndex(); });
    bind_tab.def("setCurrentIndex", [](JZTabWidget* t, int i) { t->setCurrentIndex(i); });
    bind_tab.def("onCurrentChanged", [this](JZTabWidget* t, JSValue cb) { bindSignal(t->currentChanged, cb); });
    setWidgetCreate(bind_tab);
    m_engine->registClass(bind_tab.classDefine());

    // JZSplitter —— 可拖分隔容器(方向: 0=Horizontal 1=Vertical, 构造实参被忽略 → 用 setOrientation)
    jsbind::ClassBind<JZSplitter> bind_split("JZSplitter", bind_w);
    bind_split.def("setOrientation", [](JZSplitter* s, int o) { s->setOrientation((JZSplitter::Orientation)o); });
    bind_split.def("orientation", [](JZSplitter* s) { return (int)s->orientation(); });
    bind_split.def("addWidget", [this](JZSplitter* s, JSValue w) {
        s->addWidget(takeWidget(s, w, "JZSplitter.addWidget"));
    });
    bind_split.def("insertWidget", [this](JZSplitter* s, int i, JSValue w) {
        s->insertWidget(i, takeWidget(s, w, "JZSplitter.insertWidget"));
    });
    bind_split.def("removeWidget", [](JZSplitter* s, JZWidget* w) { s->removeWidget(w); });
    bind_split.def("count", [](JZSplitter* s) { return s->count(); });
    bind_split.def("setStretchFactor", [](JZSplitter* s, int i, int v) { s->setStretchFactor(i, v); });
    bind_split.def("setHandleWidth", [](JZSplitter* s, int w) { s->setHandleWidth(w); });
    bind_split.def("handleWidth", [](JZSplitter* s) { return s->handleWidth(); });
    bind_split.def("setCollapsible", [](JZSplitter* s, int i, bool c) { s->setCollapsible(i, c); });
    bind_split.def("onSplitterMoved", [this](JZSplitter* s, JSValue cb) { bindSignal(s->sigSplitterMoved, cb); });
    setWidgetCreate(bind_split);
    m_engine->registClass(bind_split.classDefine());

    // JZScrollBar —— 独立滚动条(方向: 0=Horizontal 1=Vertical)
    jsbind::ClassBind<JZScrollBar> bind_sbar("JZScrollBar", bind_w);
    bind_sbar.def("setOrientation", [](JZScrollBar* s, int o) { s->setOrientation((JZScrollBar::Orientation)o); });
    bind_sbar.def("setRange", [](JZScrollBar* s, int mi, int ma) { s->setRange(mi, ma); });
    bind_sbar.def("minimum", [](JZScrollBar* s) { return s->minimum(); });
    bind_sbar.def("maximum", [](JZScrollBar* s) { return s->maximum(); });
    bind_sbar.def("setValue", [](JZScrollBar* s, int v) { s->setValue(v); });
    bind_sbar.def("value", [](JZScrollBar* s) { return s->value(); });
    bind_sbar.def("setSingleStep", [](JZScrollBar* s, int v) { s->setSingleStep(v); });
    bind_sbar.def("singleStep", [](JZScrollBar* s) { return s->singleStep(); });
    bind_sbar.def("setPageStep", [](JZScrollBar* s, int v) { s->setPageStep(v); });
    bind_sbar.def("pageStep", [](JZScrollBar* s) { return s->pageStep(); });
    bind_sbar.def("onValueChanged", [this](JZScrollBar* s, JSValue cb) { bindSignal(s->sigValueChanged, cb); });
    setWidgetCreate(bind_sbar);
    m_engine->registClass(bind_sbar.classDefine());

    // JZScrollArea —— 滚动区(setWidget 的内容控件由滚动区接管生命周期)
    jsbind::ClassBind<JZScrollArea> bind_area("JZScrollArea", bind_w);
    // 内容控件由滚动区接管生命周期 ⇒ 形参收 JSValue 把 JS 侧包装翻成 C++ 拥有(/Transfer/)
    // 并登记保活(父控件 = 本滚动区; setWidget 内部会把它挂到内部 viewport 下, 属本控件的对象树)
    bind_area.def("setWidget", [this](JZScrollArea* a, JSValue c) {
        a->setWidget(takeWidget(a, c, "JZScrollArea.setWidget"));
    });
    bind_area.def("widget", [](JZScrollArea* a) { return a->widget(); }, jsbind::returnOwnedByCpp);
    bind_area.def("setContentSize", [](JZScrollArea* a, int w, int h) { a->setContentSize(w, h); });
    bind_area.def("scrollTo", [](JZScrollArea* a, int x, int y) { a->scrollTo(x, y); });
    bind_area.def("scrollX", [](JZScrollArea* a) { return a->scrollX(); });
    bind_area.def("scrollY", [](JZScrollArea* a) { return a->scrollY(); });
    bind_area.def("viewportWidth", [](JZScrollArea* a) { return a->viewportWidth(); });
    bind_area.def("viewportHeight", [](JZScrollArea* a) { return a->viewportHeight(); });
    bind_area.def("verticalScrollBar", [](JZScrollArea* a) { return a->verticalScrollBar(); }, jsbind::returnOwnedByCpp);
    bind_area.def("horizontalScrollBar", [](JZScrollArea* a) { return a->horizontalScrollBar(); }, jsbind::returnOwnedByCpp);
    setWidgetCreate(bind_area);
    m_engine->registClass(bind_area.classDefine());

    // JZImageLabel —— 图片显示(图片像素/缩放/旋转)
    jsbind::ClassBind<JZImageLabel> bind_imglab("JZImageLabel", bind_w);
    bind_imglab.def("setImage", [](JZImageLabel* l, JZImage* img) {
        if (!img)
            throw std::runtime_error("JZImageLabel.setImage: image 不能为空");
        l->setImage(*img);
    });
    bind_imglab.def("image", [](JZImageLabel* l) { return l->image(); });
    bind_imglab.def("setScale", [](JZImageLabel* l, int zoom) { l->setScale((uint16_t)zoom); });
    bind_imglab.def("setRotation", [](JZImageLabel* l, int angle) { l->setRotation(angle); });
    setWidgetCreate(bind_imglab);
    m_engine->registClass(bind_imglab.classDefine());
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
    // 父控件的控件表里(保活它的 JS 包装) —— 两件事都在 module->takeWidget 里做。
    // 父控件 = 布局的容器(apply 时记下)。尚未 setLayout 时容器为空: 此时只能转移所有权,
    // 登记等 apply 把控件挂到容器下之后再说(那种写法请改用 w.layout().addWidget(...))。
    bind.def("addWidget", [module](Layout* l, JSValue w, int stretch) {
        JZWidget* parent = l->container();
        JZWidget* child = parent ? module->takeWidget(parent, w, "layout.addWidget")
                                 : jsTakeWidgetArg(module->engine()->context(), w, "layout.addWidget");
        l->addWidget(child, stretch);
    });
    bind.def("addSpacing", [](Layout* l, int s) { l->addSpacing(s); });
    bind.def("addStretch", [](Layout* l, int s) { l->addStretch(s); });
    bind.def("insertWidget", [module](Layout* l, int i, JSValue w, int stretch) {
        JZWidget* parent = l->container();
        JZWidget* child = parent ? module->takeWidget(parent, w, "layout.insertWidget")
                                 : jsTakeWidgetArg(module->engine()->context(), w, "layout.insertWidget");
        l->insertWidget(i, child, stretch);
    });
    bind.def("removeAt", [](Layout* l, int i) { l->removeAt(i); });
    bind.def("removeWidget", [](Layout* l, JZWidget* w) { l->removeWidget(w); });
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
    m_engine->registClass(bind_vbox.classDefine());
    
    jsbind::ClassBind<JZHBoxLayout> bind_hbox("JZHBoxLayout", bind_layout);
    defBoxLayoutMethods(bind_hbox, this);
    m_engine->registClass(bind_hbox.classDefine());

    // JZGridLayout
    jsbind::ClassBind<JZGridLayout> bind_grid("JZGridLayout", bind_layout);
    // 父控件 = 布局的容器(尚未 setLayout 时为空, 那种写法只能转所有权); 与盒子布局同一套
    bind_grid.def("addWidget", [this](JZGridLayout* l, JSValue w, int row, int col) {
        JZWidget* parent = l->container();
        JZWidget* child = parent ? takeWidget(parent, w, "JZGridLayout.addWidget")
                                 : jsTakeWidgetArg(m_engine->context(), w, "JZGridLayout.addWidget");
        l->addWidget(child, row, col);
    });
    bind_grid.def("addWidgetSpan", [this](JZGridLayout* l, JSValue w, int r, int c, int rs, int cs) {
        JZWidget* parent = l->container();
        JZWidget* child = parent ? takeWidget(parent, w, "JZGridLayout.addWidgetSpan")
                                 : jsTakeWidgetArg(m_engine->context(), w, "JZGridLayout.addWidgetSpan");
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
    bind_grid.def("removeWidget", [](JZGridLayout* l, JZWidget* w) { l->removeWidget(w); });
    bind_grid.def("clear", [](JZGridLayout* l) { l->clear(); });
    m_engine->registClass(bind_grid.classDefine());

    // JZSelfLayout
    jsbind::ClassBind<JZSelfLayout> bind_self("JZSelfLayout", bind_layout);
    bind_self.def("addWidget", [this](JZSelfLayout* l, JSValue w, int anchor, int dx, int dy) {
        JZWidget* parent = l->container();
        JZWidget* child = parent ? takeWidget(parent, w, "JZSelfLayout.addWidget")
                                 : jsTakeWidgetArg(m_engine->context(), w, "JZSelfLayout.addWidget");
        l->addWidget(child, (JZSelfLayout::Anchor)anchor, dx, dy);
    });
    bind_self.def("setAnchor", [](JZSelfLayout* l, JZWidget* w, int anchor, int dx, int dy) { l->setAnchor(w, (JZSelfLayout::Anchor)anchor, dx, dy); });
    bind_self.def("removeWidget", [](JZSelfLayout* l, JZWidget* w) { l->removeWidget(w); });
    bind_self.def("removeAt", [](JZSelfLayout* l, int i) { l->removeAt(i); });
    bind_self.def("clear", [](JZSelfLayout* l) { l->clear(); });
    bind_self.def("count", [](JZSelfLayout* l) { return l->count(); });
    m_engine->registClass(bind_self.classDefine());
}

// ---------- JZPainter (绘制类, 非事件转发) ----------

void JZJSWidgetModule::registPainter()
{
    jsbind::ClassBind<JZPainter> bind_p("JZPainter");
    // 创建请求全部转发给模块持有的 JZJSPaintScope(动态转发 + 登记 + 生命周期):
    //   new JZPainter(widget) -> 屏幕画笔, 等价 C++ 的栈对象 `JZPainter p(this)`, 只在本次 onPaint 内有效
    //   new JZPainter(image)  -> 离屏画笔, 自持图层, 析构(或 p.end())时把内容合成回 image
    // 登记/失效只留包装的**共享状态**(JSRawPointer<JZPainter>::Info), 不需要 setInstanceBind 之类的
    // 创建后钩子: createArgs 阶段本就拿不到 JSValue, 也不必拿到 —— 状态独立于 JS 对象存活,
    // 释放点在 paintEvent 末尾(JZJSPaintScope::Guard → end()), 那里用状态就能完成 delete + 失效。
    // 包装模型: opaque 必须是 JSRawPointer<JZPainter>*(createValue 直接塞进 JS 对象),
    // 直接返回底层指针会被后续按 JSPointerBase* 访问 = UB。详见 JZJSPaintScope.h。
    bind_p.setCreate([this](JSContext* ctx, int argc, JSValueConst* argv) -> jsbind::JSPointerBase* {
        return m_paintScope.createPainter(ctx, argc, argv);
    });
    // 显式结束绘制: 离屏模式立即把图层合成回 image(之后 image 才可用), 屏幕模式提前补 Pop。
    bind_p.def("end", [this](jsbind::JSRawPointer<JZPainter>& p) { m_paintScope.endPainter(p); });
    bind_p.def("save", [](JZPainter* p) { p->save(); });
    bind_p.def("restore", [](JZPainter* p) { p->restore(); });
    bind_p.def("translate", [](JZPainter* p, double dx, double dy) { p->translate(dx, dy); });
    bind_p.def("rotate", [](JZPainter* p, double deg) { p->rotate(deg); });
    bind_p.def("scale", [](JZPainter* p, double sx, double sy) { p->scale(sx, sy); });
    bind_p.def("resetTransform", [](JZPainter* p) { p->resetTransform(); });
    bind_p.def("setClip", [](JZPainter* p, int x, int y, int w, int h) { p->setClip(JZRect(x, y, w, h)); });
    bind_p.def("setPen", [](JZPainter* p, const std::string& color, int width) { p->setPen(JZPen(jzParseColor(color), width)); });
    bind_p.def("setBrush", [](JZPainter* p, const std::string& color) { p->setBrush(JZBrush(jzParseColor(color))); });
    bind_p.def("setFont", [](JZPainter* p, const std::string& family, int size) { p->setFont(JZFont(family, size)); });
    bind_p.def("drawLine", [](JZPainter* p, int x1, int y1, int x2, int y2) { p->drawLine(x1, y1, x2, y2); });
    bind_p.def("drawRect", [](JZPainter* p, int x, int y, int w, int h) { p->drawRect(x, y, w, h); });
    bind_p.def("drawRoundRect", [](JZPainter* p, int x, int y, int w, int h, int r) { p->drawRoundRect(x, y, w, h, r); });
    bind_p.def("fillRect", [](JZPainter* p, int x, int y, int w, int h, const std::string& color) { p->fillRect(x, y, w, h, jzParseColor(color)); });
    bind_p.def("fillRoundRect", [](JZPainter* p, int x, int y, int w, int h, int r, const std::string& color) { p->fillRoundRect(x, y, w, h, r, jzParseColor(color)); });
    bind_p.def("drawEllipse", [](JZPainter* p, int x, int y, int w, int h) { p->drawEllipse(x, y, w, h); });
    bind_p.def("fillEllipse", [](JZPainter* p, int x, int y, int w, int h, const std::string& color) { p->fillEllipse(x, y, w, h, jzParseColor(color)); });
    bind_p.def("drawText", [](JZPainter* p, int x, int y, const std::string& text) { p->drawText(x, y, text.c_str()); });
    bind_p.def("drawImage", [](JZPainter* p, JZImage* img, int x, int y, int w, int h) {
        if (!img)
            throw std::runtime_error("drawImage: image 不能为空");
        p->drawImage(*img, x, y, w, h);
    });
    bind_p.def("setTransform", [](JZPainter* p, double a, double b, double c_, double d, double e, double f) {
        JZMatrix2D m;
        m.setMatrix(a, b, c_, d, e, f);
        p->setTransform(m);
    });
    m_engine->registClass(bind_p.classDefine());
}

// 从 createUi 返回的 { root, <id>: widget, __keep } 取出 root 窗口, 并把 root 之外的全部属性
// (含 id 子控件与 __keep 保活数组)挂到 root 上, 使 ui 既是 JZWindow, 又能通过 ui.<id> 访问子控件。
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

// jzWidget.setStyleSheet(css): 设置**全局**样式表(等价 C++ 的 jzApp->setStyleSheet)。
// 换肤 = 整表替换: 变量只有 :root 一处, 且是 parse 期文本替换(见 JZWidget 样式指南),
// 所以切主题要把两份 CSS 各完整给一次, 而不是"改一个变量"。
static JSValue js_jzWidget_setStyleSheet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc != 1)
        return JS_ThrowTypeError(ctx, "jzWidget.setStyleSheet(css) 需要 1 个参数");

    const char* s = JS_ToCString(ctx, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    std::string css(s);
    JS_FreeCString(ctx, s);

    if (!jzApp)
        return JS_ThrowInternalError(ctx, "jzWidget.setStyleSheet: 应用对象不存在(JZApplication 未创建)");
    if (!jzApp->setStyleSheet(css))
        return JS_ThrowSyntaxError(ctx, "样式表语法错误: %s", jzApp->styleSheetError().c_str());
    return JS_TRUE;
}

static JSValue js_jzWidget_styleSheetError(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    if (!jzApp)
        return JS_NewString(ctx, "");
    return JS_NewString(ctx, jzApp->styleSheetError().c_str());
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

    // 全局 jzWidget: 目前只收口"应用级"入口 —— 全局样式表(JZApplication::setStyleSheet)。
    // 单控件级样式用 widget.setStyleSheet(css)。
    JSValue jzWidget = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, jzWidget, "setStyleSheet", JS_NewCFunction(ctx, js_jzWidget_setStyleSheet, "setStyleSheet", 1));
    JS_SetPropertyStr(ctx, jzWidget, "styleSheetError", JS_NewCFunction(ctx, js_jzWidget_styleSheetError, "styleSheetError", 0));
    m_engine->setProperty(m_engine->globalObject(), "jzWidget", jzWidget);
}