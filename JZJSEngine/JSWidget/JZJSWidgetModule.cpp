#include "JZJSWidgetModule.h"

#include "JZJSBind.h"
#include "JZJSWidget.h"
#include "JZApplication.h"
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

// ---------- JS 事件转发(模块自己当过滤器; 见 JZJSWidgetModule.h) ----------
// 回调存在 C++ 侧(WidgetInfo::callbacks), 派发时按槽查 —— 与"哪份包装挂了回调"无关。

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

void JZJSWidgetModule::setSlot(JZObject* object, const char* name, JSValueConst cb)
{
    if (!m_engine || !object || !name)
        return;
/*
    JSContext* ctx = m_engine->context();

    // null / undefined = 清空(对应 JS 侧 `w.onPaint = null` / `btn.onClicked = null`); 其它必须是函数
    bool clear = JS_IsNull(cb) || JS_IsUndefined(cb);
    if (!clear && !JS_IsFunction(ctx, cb))
        throw std::runtime_error(std::string(name) + ": 需要一个函数(或 null 清空)");

    WidgetInfo* node = findWidgetInfo(object);
    if (!node)
        return;

    auto it = node->slots.find(name);
    if (clear)
    {
        if (it != node->slots.end())
        {
            SlotValue old = it->second;
            node->slots.erase(it);
            m_engine->releaseCallback(old.rootId, old.value);
        }
        return;
    }

    // 先 dup 新的、再放旧的(顺序不能反: 赋同一个函数时不会先释后取)。
    // 同时在引擎的回调 GC 根上登记一份 —— 回调闭包常与它捕获的 JS 包装成环(包装 -> C++ 对象
    // -> 槽 -> 闭包 -> 包装), 只被 C++ 持有的话 quickjs 的循环 GC 会把它回收(见 retainCallback)。
    SlotValue old;
    if (it != node->slots.end())
        old = it->second;

    int rootId = m_engine->retainCallback(cb);
    node->slots[name] = SlotValue(rootId, JS_DupValue(ctx, cb));

    if (!JS_IsUndefined(old.value))
        m_engine->releaseCallback(old.rootId, old.value);
*/
}

JSValue JZJSWidgetModule::slotValue(const WidgetInfo& node, const char* name) const
{
//    auto it = node.slots.find(name);
//    return (it == node.slots.end()) ? JS_UNDEFINED : it->second.value;
    return JS_UNDEFINED;
}

JSValue JZJSWidgetModule::slot(JZObject* object, const char* name)
{
    if (!m_engine || !object || !name)
        return JS_UNDEFINED;
    WidgetInfo* node = findWidgetInfo(object);
    if (!node)
        return JS_UNDEFINED;
    return slotValue(*node, name);
}

JSValue JZJSWidgetModule::slotToJs(JZObject* object, const char* name)
{
    JSValue cb = slot(object, name);
    return JS_IsUndefined(cb) ? JS_NULL : JS_DupValue(m_engine->context(), cb);
}

// 以该控件的包装为 this 调回调; 事件对象包成**借用**包装(事件是栈对象, 只在本次派发期间有效)
template <class E>
void JZJSWidgetModule::forwardJs(WidgetInfo& node, const char* name, E* event)
{
    JSContext* ctx = m_engine->context();
    jsbind::JSRawPointer<E> ref(m_engine, event, true);   // cppOwner=true: JS 不负责释放(栈对象)
    std::vector<JZJSValuePtr> params;
    params.push_back(m_engine->makeValuePtr(toJSValue(ctx, ref)));

    // this = 该控件的包装(回调里 this 仍是控件, self() 也靠这层压栈)。
    // 登记里还没有包装句柄时(如 C++ 侧建好、只是被 JS 拿到过句柄的控件)现造一份:
    // cppOwner=true —— JS 只持句柄, GC 不会去动底层对象。
    JSValue thisVal = m_engine->toJsValueFromPtr(static_cast<JZWidget*>(node.object), true);
    m_engine->callMemberFunction(name, JZJSValuePtr(ctx, thisVal), params);
    ref.clear();   // 派发结束: JS 侧若还留着该事件对象, 访问它会失效而不是读悬垂内存
}

template <class E>
bool JZJSWidgetModule::forwardIf(WidgetInfo& node, const char* name, E* event)
{
    if (JS_IsUndefined(slotValue(node, name)))
        return false;   // 没设回调 -> 放行走原生实现
    forwardJs(node, name, event);
    return true;
}

bool JZJSWidgetModule::eventFilter(JZObject* watched, JZEvent* event)
{
    if (!m_engine || !watched || !event)
        return false;

    // 控件 -> 状态(含事件回调槽)的关联在控件登记表里
    WidgetInfo* node = findWidgetInfo(watched);
    if (!node)
        return false;

    // 事件类型 -> 回调槽(槽名见 eventSlotName)。返回 true = 已转给 JS, 事件被拦下(不再走原生实现)。
    // 注: 槽表按"名字"索引(事件与信号共用一张), 所以这里把枚举翻成名字再查。
    switch (event->type())
    {
    case JZEvent_paint:
        // JS 侧画笔(new JZPainter(this))的生命周期 = 本次 onPaint 派发(对应 C++ 的栈对象
        // `JZPainter p(this)`): 出作用域立即回收, 让绘制流的 Push/Pop 同帧配平。
        if (JS_IsUndefined(slotValue(*node, eventSlotName(Slot_paint))))
            return false;
        {
            beginPainter((JZWidget*)watched);
            forwardJs(*node, eventSlotName(Slot_paint), static_cast<JZPainterEvent*>(event));
            endPainter();
        }
        return true;
    case JZEvent_mouse:
    {
        JZMouseEvent* me = static_cast<JZMouseEvent*>(event);
        switch (me->mouseType())
        {
        case JZMouseEvent::Press:   return forwardIf(*node, eventSlotName(Slot_mousePress), me);
        case JZMouseEvent::Release: return forwardIf(*node, eventSlotName(Slot_mouseRelease), me);
        case JZMouseEvent::Move:    return forwardIf(*node, eventSlotName(Slot_mouseMove), me);
        case JZMouseEvent::Wheel:   return forwardIf(*node, eventSlotName(Slot_wheel), me);
        }
        return false;
    }
    case JZEvent_enter:    return forwardIf(*node, eventSlotName(Slot_enter), event);
    case JZEvent_leave:    return forwardIf(*node, eventSlotName(Slot_leave), event);
    case JZEvent_key:
    {
        JZKeyEvent* ke = static_cast<JZKeyEvent*>(event);
        return ke->isPress() ? forwardIf(*node, eventSlotName(Slot_keyPress), ke)
                             : forwardIf(*node, eventSlotName(Slot_keyRelease), ke);
    }
    case JZEvent_focusIn:  return forwardIf(*node, eventSlotName(Slot_focusIn), event);
    case JZEvent_focusOut: return forwardIf(*node, eventSlotName(Slot_focusOut), event);
    case JZEvent_show:     return forwardIf(*node, eventSlotName(Slot_show), static_cast<JZShowEvent*>(event));
    case JZEvent_hide:     return forwardIf(*node, eventSlotName(Slot_hide), static_cast<JZHideEvent*>(event));
    case JZEvent_resize:   return forwardIf(*node, eventSlotName(Slot_resize), static_cast<JZResizeEvent*>(event));
    default:
        return false;
    }
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
    }

    registGeometry();
    registColor();
    registImage();
    registEvents();
    registWidgets();
    registDialogs();
    registSettings();
    registItems();
    registViews();
    registApp();
    registLayouts();
    registPens();
    registMatrix();
    registPainter();
    registUiLoader();

    m_typeWindow = m_engine->ctypeidToType(typeid(JZWindow).name());    
}

void JZJSWidgetModule::deinitEngine()
{
    m_widgetData = JS_UNDEFINED;   // 整数句柄, 无引用计数, 直接丢

    if (m_engine)
        m_engine->setWidgetModule(nullptr);

    m_engine = nullptr;
    m_typeWindow = -1;    
}

jsbind::JSPointerBase* JZJSWidgetModule::createPainter(JSContext* ctx, int argc, JSValueConst* argv)
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
        auto* painter = new JZPainter(*img);
        return new jsbind::JSRawPointer<JZPainter>(m_engine, painter);
    }
    else if(m_engine->checkType(argv[0], "JZWidget"))   // 校验在前(构造参数没有类型表, 得自己先校验)
    {
        JZWidget* w = fromJSValue<JZWidget*>(ctx, argv[0]);
        if(w != m_paintWidget)
        {
            JS_ThrowTypeError(ctx, "new JZPainter(target): 需要 JZWidget 或 JZImage 参数");
            return nullptr;
        }
        if (m_painter)
        {
            JS_ThrowTypeError(ctx, "不能创建多个JZPainter");
            return nullptr;
        }

        auto* wrapper = new jsbind::JSRawPointer<JZPainter>(m_engine, new JZPainter(w));        
        m_painter = wrapper;
        return wrapper;
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
        delete m_painter->get();
        m_painter->clear();
        m_painter = nullptr;
    }
    m_paintWidget = nullptr;
}

// ---------- 控件登记 (JS 包装保活) ----------
// 见 JZJSWidgetModule.h 顶部说明: 控件挂进对象树后, 模块持一份它的 JS 包装(保活),
// 控件销毁(或脱离父树)时释放; 顶层控件不保活(生死由 JS 决定)。
// 登记节点按**控件指针**索引, 但它不在模块自己的表里: 挂在引擎身份表那份共享状态(JZJSEngine::
// pointerStateFor)的 data 槽上(JSPointerState::data)。键同样是控件裸指针, 与控件树结构无关 ——
// 改父 / 脱离父树都只更新这一条, 不需要搬节点; 控件销毁(sigDestroyed)时"先摘节点再放引用"。
// 之所以共用引擎那张表: 登记表与"对象身份/所有权"本来就该是一份, 免得两处各存一份状态再互相同步。

// 节点析构 = 释放它持有的 JS 引用(保活包装 + 回调槽)。只在 removeWidgetInfo 里被 delete, 那时
// ctx 还活着; engine 为空说明引擎已经走了, 不能再碰 JSValue。
JZJSWidgetModule::WidgetInfo::~WidgetInfo()
{
    if (!engine)
        return;
/*
    JSContext* ctx = engine->context();

    // 保活的那份包装(dup): 控件已被对象树 delete(或交回 JS)时才走到这里, 包装的守卫指针已置空,
    // 所以不会二次删除。
    if (!JS_IsUndefined(jsvalue))
    {
        JSValue held = jsvalue;
        jsvalue = JS_UNDEFINED;   // 先清再放: 级联路径重入不会二次释放
        JS_FreeValue(ctx, held);
    }
    jsRef = JS_UNDEFINED;   // 借用型, 只需丢掉指针(不释放)

    // 回调槽(事件 + 信号): 模块持有的那份引用 + 它在引擎回调 GC 根上的登记 id,
    // 一起交回引擎释放(置空根数组对应项, 再 free 这份引用)。
    for (auto& kv : slots)
    {
        if (!JS_IsUndefined(kv.second.value))
        {
            SlotValue sv = kv.second;
            kv.second = SlotValue();
            engine->releaseCallback(sv.rootId, sv.value);
        }
    }
    slots.clear();

    ext.reset();
    object = nullptr;
*/
}

JZJSWidgetModule::WidgetInfo* JZJSWidgetModule::findWidgetInfo(JZObject* object)
{
    if (!m_engine || !object)
        return nullptr;

    // 节点挂在引擎身份表那份状态的 data 槽上(键 = 控件裸指针)。该对象没被包装过就没有状态。
    auto ptr = m_engine->getPointer(object);
    if (!ptr)
        return nullptr;

    auto state = ptr->state();
    if (!state)
        return nullptr;

    // 状态上的 data 不一定是我们挂的(引擎那张表是共用的) —— 只有是本模块的节点才认。
    return dynamic_cast<WidgetInfo*>(state->data.get());
}

void JZJSWidgetModule::onWidgetCreate(jsbind::JSPointerBase *ptr, JZWidget* object)
{    
    object->installEventFilter(this);

    std::unique_ptr<WidgetInfo> info(new WidgetInfo());
    info->object = object;
    info->engine = m_engine;    
    ptr->state()->data = std::move(info);
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

// ---------- 信号 -> 回调槽 ----------
// 信号在 JS 侧就是**属性**(`btn.onClicked = fn`): 属性 setter 写槽 + ensureConnected(只连一次);
// 连接里的 lambda 每次去查槽 —— 槽空(被赋值 null 清空过)就跳过, 所以不需要"断开连接"。

JSValue JZJSWidgetModule::callJsCallback(JSValueConst fn, JZObject* owner, int argc, JSValueConst* argv)
{
/*
    JSContext* ctx = m_engine->context();

    // 把"发信号的对象"压成当前 this: 回调写成箭头函数时也能用 self() 认出自己是谁。
    // 控件/窗口在控件登记树里有包装(jsRef); 没有(非控件对象 / 已摘节点)就压 undefined -> self() 给 null。
    JSValue self_val = JS_UNDEFINED;
    if (owner)
    {
        WidgetInfo* node = findWidgetInfo(owner);
        if (node)
            self_val = node->jsRef;
    }
    JZJSEngine::SelfScope self_scope(m_engine, self_val);

    // 调用期间自己持一份: 回调里若把同一个信号属性重设/清空(槽里的旧值会被 release),
    // 正在执行的这个函数对象仍然是活的 —— 否则就是"调用中把自己释放掉"的 UAF。
    JSValue held = JS_DupValue(ctx, fn);

    // 回调里抛出的异常不能跨 quickjs 的 C 调用栈传回去(信号是 C++ 侧同步 emit 的),
    // 按引擎既有口径: 交给 reportUncaughtError 并入错误链(sigError/sigLog)。
    JSValue ret = JS_Call(ctx, held, JS_UNDEFINED, argc, argv);
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
*/
    return JS_UNDEFINED;
}

// ---------- 信号属性的注册样板 ----------
// 与事件属性共用同一张槽表; 区别只是信号要"首次赋值时 connect 一次"(事件靠事件过滤器派发)。
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
    m_engine->registClass(bind_point.classDefine());

    jsbind::ClassBind<JZSize> bind_size("JZSize");
    bind_size.def("setWidth", [](JZSize* s, int w) { s->setWidth(w); });
    bind_size.def("setHeight", [](JZSize* s, int h) { s->setHeight(h); });
    bind_size.def("width", [](JZSize* s) { return s->width(); });
    bind_size.def("height", [](JZSize* s) { return s->height(); });
    bind_size.def("isEmpty", [](JZSize* s) { return s->isEmpty(); });
    bind_size.def("isValid", [](JZSize* s) { return s->isValid(); });
    bind_size.def("equal", [](JZSize* s, const JZSize& o) { return *s == o; });
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

// 校验"是控件(或其子类)"并取裸指针(不翻转所有权)。出错抛 std::runtime_error
// (由 JZJSEngine::call 的 try/catch 转成 JS 异常)。
static JZWidget* jsWidgetArg(JSContext* ctx, JSValueConst value, const char* what)
{
    if (!js_check_class_arg<JZWidget>(ctx, value))
        throw std::runtime_error(std::string(what) + ": 需要 JZWidget(或其子类)");
    return fromJSValue<JZWidget*>(ctx, value);
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
        if (!js_check_class_arg<JZWidget>(ctx, argv[0]))
        {
            JS_ThrowTypeError(ctx, "%s: 第 1 个参数需要 JZWidget(或其子类), 也可以传 null", what);
            return false;
        }
        parent = fromJSValue<JZWidget*>(ctx, argv[0]);
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
    if (!js_check_class_arg<JZWidget>(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "%s: 第 1 个参数需要 JZWidget(或其子类)", what);
    if (!engine->checkType(argv[1], JSType_string) || !engine->checkType(argv[2], JSType_string))
        return JS_ThrowTypeError(ctx, "%s: title / text 需要字符串", what);

    JZWidget* parent = fromJSValue<JZWidget*>(ctx, argv[0]);
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
    
    bind_dlg.def("exec", [](JZDialog* d) { return d->exec(); });
    bind_dlg.def("accept", [](JZDialog* d) { d->accept(); });
    bind_dlg.def("reject", [](JZDialog* d) { d->reject(); });
    bind_dlg.def("result", [](JZDialog* d) { return (int)d->result(); });
    // 对话框信号: 属性形(单发/覆盖式); onFinished 带 result(int)
    //defSignalProperty(bind_dlg, "onAccepted", &JZDialog::sigAccepted);
    //defSignalProperty(bind_dlg, "onRejected", &JZDialog::sigRejected);
    //defSignalProperty(bind_dlg, "onFinished", &JZDialog::sigFinished);
    m_engine->registClass(bind_dlg.classDefine());

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

static JSValue variantToJSValue(JSContext* ctx, const JZVariant& var)
{
    switch (var.typeId())
    {
    case JZMetaType::None:   return JS_NULL;
    case JZMetaType::Bool:   return JS_NewBool(ctx, var.value<bool>());
    case JZMetaType::Int16:  return JS_NewInt32(ctx, (int)var.value<int16_t>());
    case JZMetaType::UInt16: return JS_NewInt32(ctx, (int)var.value<uint16_t>());
    case JZMetaType::Int:    return JS_NewInt32(ctx, var.value<int>());
    case JZMetaType::UInt:   return JS_NewInt32(ctx, (int32_t)var.value<unsigned int>());
    case JZMetaType::Int64:  return JS_NewInt64(ctx, var.value<int64_t>());
    case JZMetaType::UInt64: return JS_NewInt64(ctx, (int64_t)var.value<uint64_t>());
    case JZMetaType::Float:  return JS_NewFloat64(ctx, (double)var.value<float>());
    case JZMetaType::Double: return JS_NewFloat64(ctx, var.value<double>());
    case JZMetaType::String: return JS_NewString(ctx, var.value<std::string>().c_str());
    case JZMetaType::ByteArray:
    {
        JZByteArray bytes = var.value<JZByteArray>();
        return JS_NewUint8ArrayCopy(ctx, (const uint8_t*)bytes.data(), (size_t)bytes.size());
    }
    case JZMetaType::VariantList:
    {
        JZVariantList list = var.value<JZVariantList>();
        JSValue arr = JS_NewArray(ctx);
        for (size_t i = 0; i < list.size(); i++)
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, variantToJSValue(ctx, list[i]));   // 转移所有权
        return arr;
    }
    case JZMetaType::VariantMap:
    {
        JZVariantMap map = var.value<JZVariantMap>();
        JSValue obj = JS_NewObject(ctx);
        for (JZVariantMap::iterator it = map.begin(); it != map.end(); ++it)
            JS_SetPropertyStr(ctx, obj, it->first.c_str(), variantToJSValue(ctx, it->second));
        return obj;
    }
    default:
        break;
    }

    // setProperty 存进来的 JS 值(数组/对象/函数/实例): 原样还回**同一份**对象(引用计数 +1 交给 JS)
    if (var.typeId() == jzMetaTypeId<JZJSValuePtr>())
    {
        JZJSValuePtr held = var.value<JZJSValuePtr>();
        return JS_DupValue(ctx, held.get());
    }

    throw std::runtime_error("property: 该属性值无法转成 JS(只支持标量/字符串/字节串/数组/对象/JS 值)");
}

void JZJSWidgetModule::registWidgets()
{
    // 基类 JZWidget —— 裸类对象; 事件转发靠 JZSWidgetEventFilter(见 JZJSWidgetModule.h)
    jsbind::ClassBind<JZWidget> bind_w("JZWidget");
    // 事件回调属性: JS 侧写法与以前完全一样(`w.onPaint = fn` / `w.onPaint = null` 清空), 但回调
    // **存在模块这边**(见 WidgetInfo::slots / setSlot) —— 与"回调挂在哪份包装上"彻底解耦:
    // getter 造出来的临时包装、包装被 GC、包装被换掉, 都不再影响事件转发。
    // 信号(onClicked 等)用的是**同一张槽表**, 只是注册在各控件类上(见下面各 bind_xxx 的 defProperty)。
    for (int i = 0; i < EventSlot_count; i++)
    {
        const char* name = eventSlotName(i);   // 字符串字面量(静态存储期), 可安全被 lambda 捕获
        bind_w.defProperty<JSValue>(std::string(name),
            std::function<JSValue(JZWidget*)>([this, name](JZWidget* w) -> JSValue {
                return slotToJs(w, name);
            }),
            std::function<void(JZWidget*, JSValue)>([this, name](JZWidget* w, JSValue cb) {
                setSlot(w, name, cb);
            }));
    }

    // 属性系统(JZObject::setProperty): JS 侧 `w.setProperty(name, value)` —— 参数收 **JSValue**。
    // 标量走 JZVariant 自己的类型(int/double/string/bool; null/undefined = **移除**该属性); 其余
    // (数组/对象/函数/实例)用注册的 metatype(JZJSValueHolder, 见头文件)**原样**装进 JZVariant ——
    // 不做有损快照、不改写、也不再报"不支持的类型"。值进控件的通用属性表, C++ 侧 property(name) 读回。
    bind_w.def("setProperty", [this](JZWidget* w, const std::string& name, JSValue value) {
        JSContext* ctx = m_engine->context();
        JZVariant var;
        if (JS_IsNull(value) || JS_IsUndefined(value))
            var = JZVariant();   // 空 variant -> JZObject::setProperty 据此**移除**该属性
        else if (JS_IsBool(value))
            var = JZVariant(JS_ToBool(ctx, value) != 0);
        else if (JS_IsNumber(value))
        {
            double d = 0;
            JS_ToFloat64(ctx, &d, value);
            // 整数值(且在 int32 范围内)按 int 存 —— JZVariant 里 integer 与 double 是两种类型
            var = (d >= -2147483648.0 && d <= 2147483647.0 && (double)(int)d == d) ? JZVariant((int)d) : JZVariant(d);
        }
        else if (JS_IsString(value))
        {
            const char* s = JS_ToCString(ctx, value);
            var = JZVariant(std::string(s ? s : ""));
            JS_FreeCString(ctx, s);
        }
        else
        {
            // 非标量: 用 engine 的工厂建持有壳(JZJSValuePtr 是注册的 metatype, 见 JZJSEngine.h),
            // **原样**装进 JZVariant —— 引用计数由壳内部的 shared_ptr guard 管(拷贝 = 共享同一份,
            // 最后一个引用析构时才 JS_FreeValue)。
            // ⚠ dup 不能省: makeValuePtr 是"接管所有权"的工厂(内部不再 dup, 析构时 JS_FreeValue),
            //   而 def 的 JSValue 形参是**借用**的(与 JSValueConst 语义一致, 见上面 jzJsToDoubleArray
            //   的用法) —— 不 dup 会多减一次引用计数。
            var = JZVariant::fromValue(m_engine->makeValuePtr(JS_DupValue(ctx, value)));
        }
        w->setProperty(name, var);
    });

    // 属性读取: `w.property(name)` -> JS 值(没设置 / 已被移除 -> null)。转换见 variantToJSValue;
    // 存进去的 JS 值(数组/对象/函数/实例)原样还回同一份对象。
    bind_w.def("property", [this](JZWidget* w, const std::string& name) -> JSValue {
        return variantToJSValue(m_engine->context(), w->property(name));
    });

    // 延后删除 —— 嵌套模态循环里删控件的正规做法: 事件派发期间(尤其 exec() 的模态循环里)直接把控件
    // delete 掉, 会让外层正在派发的事件对象悬垂; deleteLater 排到本轮事件处理完之后。
    bind_w.def("deleteLater", [](JZWidget* w) { w->deleteLater(); });

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
    bind_w.def("setParentWidget", [this](JZWidget *w, JZWidget* parent)->JZWidget* {
        if (w->parent())
        {
            w->setParent(parent);
            if (!parent)
            {
                auto ptr = m_engine->getPointer(w);
                ptr->moveToJs();
            }
        }
        else
        {
            w->setParent(parent);
            if (parent)
            {
                JSValue value = m_engine->self();
                jsbind::JSPointerBase *base = m_engine->getValuePointer(value);
                base->moveToCpp(value);
            }
        }
        return w;
    }, jsbind::returnOwnedByCpp);
    bind_w.def("parentWidget", &JZWidget::parentWidget, jsbind::returnOwnedByCpp);
    bind_w.def("window", &JZWidget::window, jsbind::returnOwnedByCpp);
    bind_w.def("layout", &JZWidget::layout, jsbind::returnOwnedByCpp);
    // setLayout: Qt 语义, 只能设一次(已有布局/传 null 都抛错); 换布局先 clearLayout()
    bind_w.def("setLayout", [] {

        });
    bind_w.def("clearLayout", [] {

        });
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

    auto makeWindow = [](int w, int h, const std::string& title, JZWidget* parent) {
        return new JZWindow(w, h, title, parent);
    };
    jsbind::ClassBind<JZWindow> bind_win("JZWindow", bind_w);
    bind_win.def("setTitle", [](JZWindow* w, const std::string& t) { w->setTitle(t); });
    bind_win.def("title", [](JZWindow* w) { return w->title(); });
    bind_win.def("show", [](JZWindow* w) { w->show(); });
    bind_win.def("close", [](JZWindow* w) { w->close(); });    
    m_engine->registClass(bind_win.classDefine());    

    // JZMainWindow —— 主窗口(菜单栏 + 中央控件)。构造实参只有 (parent?),
    // 没有 w/h/title; 标题/几何走 setTitle/setGeometry。
    // uiLoader 把 <widget class="MainWindow"> 映射成 JZMainWindow, 子控件用
    // setCentralWidget(至多一个中央控件) / addMenuBar(菜单栏) 挂载, 不走通用 addWidget。
    // ⚠ 基类必须传 bind_win(JZWindow), 不是 bind_w —— JZMainWindow 派生自 JZWindow,
    // 传 bind_w 会让 setTitle/setGeometry/show/close 这些**窗口方法在它身上全部缺失**
    // (uiLoader 生成 `__w0.setTitle(...)` 直接 "not a function")。
    jsbind::ClassBind<JZMainWindow> bind_main("JZMainWindow", bind_win);    
    bind_main.def("setCentralWidget", [this](JZMainWindow* w, JSValue c) {
        JZWidget* child = takeWidgetFromJs(c);
        w->setCentralWidget(child);
    });
    bind_main.def("addMenuBar", [this](JZMainWindow* w, JSValue c) {
        JZWidget* mb = takeWidgetFromJs(c);
        w->addMenuBar(static_cast<JZMenuBar*>(mb));
    });
    bind_main.def("menuBar", [](JZMainWindow* w) { return w->menuBar(); }, jsbind::returnOwnedByCpp);
    m_engine->registClass(bind_main.classDefine());

    // JZMenuBar —— 菜单栏(addMenuBar 的子控件)。仅注册构造 + 实例绑定,
    // 内部菜单项(JZMenu)暂未绑定到 JS, 由 C++ 侧继续 addMenu。
    jsbind::ClassBind<JZMenuBar> bind_menubar("JZMenuBar", bind_w);    
    // JZMenuBar.addMenu: 返回 JZMenu(由菜单栏托管, JS 不拥有, 避免 GC 误删导致双重释放)
    bind_menubar.def("addMenu", [](JZMenuBar* mb, const std::string& title) {
        return mb->addMenu(title);
    }, jsbind::returnOwnedByCpp);
    bind_menubar.def("menuCount", [](JZMenuBar* mb) { return mb->menuCount(); });
    bind_menubar.def("menuAt", [](JZMenuBar* mb, int i) { return mb->menuAt(i); }, jsbind::returnOwnedByCpp);
    m_engine->registClass(bind_menubar.classDefine());

    // JZMenu —— 菜单(数据 + 弹层控制器, 非渲染树控件)。菜单项由 addAction/addMenu/addSeparator 建立。
    jsbind::ClassBind<JZMenu> bind_menu("JZMenu");
    bind_menu.def("setTitle", [](JZMenu* m, const std::string& t) { m->setTitle(t); });
    bind_menu.def("title", [](JZMenu* m) { return m->title(); });
    bind_menu.def("addAction", [](JZMenu* m, const std::string& text) {
        return m->addAction(text);
    }, jsbind::returnOwnedByCpp);
    bind_menu.def("addMenu", [](JZMenu* m, const std::string& title) {
        return m->addMenu(title);
    }, jsbind::returnOwnedByCpp);
    bind_menu.def("addSeparator", [](JZMenu* m) { m->addSeparator(); });
    bind_menu.def("menuCount", [](JZMenu* m) { return m->menuCount(); });
    bind_menu.def("menuAt", [](JZMenu* m, int i) { return m->menuAt(i); }, jsbind::returnOwnedByCpp);
    bind_menu.def("actionCount", [](JZMenu* m) { return m->actionCount(); });
    bind_menu.def("actionAt", [](JZMenu* m, int i) { return m->actionAt(i); }, jsbind::returnOwnedByCpp);
    m_engine->registClass(bind_menu.classDefine());

    // JZAction —— 菜单项(数据模型)。triggered 信号转 JS 回调。
    jsbind::ClassBind<JZAction> bind_action("JZAction");
    bind_action.def("setText", [](JZAction* a, const std::string& t) { a->setText(t); });
    bind_action.def("text", [](JZAction* a) { return a->text(); });
    bind_action.def("setShortcut", [](JZAction* a, const std::string& s) { a->setShortcut(s); });
    bind_action.def("shortcut", [](JZAction* a) { return a->shortcut(); });
    bind_action.def("setEnabled", [](JZAction* a, bool on) { a->setEnabled(on); });
    bind_action.def("isEnabled", [](JZAction* a) { return a->isEnabled(); });
    bind_action.def("setCheckable", [](JZAction* a, bool on) { a->setCheckable(on); });
    bind_action.def("isCheckable", [](JZAction* a) { return a->isCheckable(); });
    bind_action.def("setChecked", [](JZAction* a, bool on) { a->setChecked(on); });
    bind_action.def("isChecked", [](JZAction* a) { return a->isChecked(); });
    bind_action.def("setIcon", [](JZAction* a, JZImage* img) { a->setIcon(*img); });
    bind_action.def("trigger", [](JZAction* a) { a->trigger(); });
    //defSignalProperty(bind_action, "onTriggered", &JZAction::triggered);
    m_engine->registClass(bind_action.classDefine());

    // JZLabel
    jsbind::ClassBind<JZLabel> bind_label("JZLabel", bind_w);
    bind_label.def("setText", &JZLabel::setText);
    bind_label.def("text", &JZLabel::text);
    bind_label.def("setAlign", [](JZLabel* w, int a) { w->setAlign(a); });    
    m_engine->registClass(bind_label.classDefine());

    // JZPushButton
    jsbind::ClassBind<JZPushButton> bind_btn("JZPushButton", bind_w);
    bind_btn.def("setText", &JZPushButton::setText);
    bind_btn.def("text", &JZPushButton::text);
    bind_btn.def("setIcon", [](JZPushButton* w, JZImage* img) { w->setIcon(*img); });
    bind_btn.def("icon", [](JZPushButton* w) { return w->icon(); });
    bind_btn.def("setWidth", [](JZPushButton* w, int v) { w->setWidth(v); });
    bind_btn.def("setHeight", [](JZPushButton* w, int v) { w->setHeight(v); });
    // 点击信号: 属性形(单发/覆盖式) —— `btn.onClicked = () => ...`; 赋 null 清空
    //defSignalProperty(bind_btn, "onClicked", &JZPushButton::sigClicked);    
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
    // 文本类信号(声明在 JZAbstractTextEdit 上, 所以成员指针的宿主类型是它 —— 模板的 S 参数)
    //defSignalProperty(bind_edit, "onTextChanged", &JZLineEdit::sigTextChanged);
    //defSignalProperty(bind_edit, "onReturnPressed", &JZLineEdit::sigReturnPressed);
    //defSignalProperty(bind_edit, "onEditingFinished", &JZLineEdit::sigEditingFinished);    
    m_engine->registClass(bind_edit.classDefine());

    // JZCheckBox
    jsbind::ClassBind<JZCheckbox> bind_check("JZCheckbox", bind_w);
    bind_check.def("setText", &JZCheckbox::setText);
    bind_check.def("text", &JZCheckbox::text);
    bind_check.def("setChecked", [](JZCheckbox* w, bool on) { w->setChecked(on); });
    bind_check.def("isChecked", [](JZCheckbox* w) { return w->isChecked(); });
    //defSignalProperty(bind_check, "onToggled", &JZCheckbox::sigToggled);   // 回调带 (bool)    
    m_engine->registClass(bind_check.classDefine());

    // JZRadioButton
    jsbind::ClassBind<JZRadioButton> bind_radio("JZRadioButton", bind_w);
    bind_radio.def("setText", &JZRadioButton::setText);
    bind_radio.def("text", &JZRadioButton::text);
    bind_radio.def("setChecked", [](JZRadioButton* w, bool on) { w->setChecked(on); });
    bind_radio.def("isChecked", [](JZRadioButton* w) { return w->isChecked(); });
    //defSignalProperty(bind_radio, "onToggled", &JZRadioButton::sigToggled);   // RadioButton 的信号**无参**    
    m_engine->registClass(bind_radio.classDefine());

    // JZSlider
    jsbind::ClassBind<JZSlider> bind_slider("JZSlider", bind_w);
    bind_slider.def("setRange", [](JZSlider* w, int min, int max) { w->setRange(min, max); });
    bind_slider.def("setValue", [](JZSlider* w, int v) { w->setValue(v); });
    bind_slider.def("value", [](JZSlider* w) { return w->value(); });
    //defSignalProperty(bind_slider, "onValueChanged", &JZSlider::sigValueChanged);   // 回调带 (int)    
    m_engine->registClass(bind_slider.classDefine());

    // JZProgressBar
    jsbind::ClassBind<JZProgressBar> bind_bar("JZProgressBar", bind_w);
    bind_bar.def("setRange", [](JZProgressBar* w, int min, int max) { w->setRange(min, max); });
    bind_bar.def("setValue", [](JZProgressBar* w, int v) { w->setValue(v); });
    bind_bar.def("value", [](JZProgressBar* w) { return w->value(); });    
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
    //defSignalProperty(bind_combo, "onCurrentIndexChanged", &JZComboBox::currentIndexChanged);   // (int)
    //defSignalProperty(bind_combo, "onCurrentTextChanged", &JZComboBox::currentTextChanged);    // (string)    
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
    //defSignalProperty(bind_spin, "onValueChanged", &JZSpinBox::sigValueChanged);   // 回调带 (int)    
    m_engine->registClass(bind_spin.classDefine());

    // JZGroupBox
    jsbind::ClassBind<JZGroupBox> bind_group("JZGroupBox", bind_w);
    bind_group.def("setTitle", [](JZGroupBox* w, const std::string& s) { w->setTitle(s); });
    bind_group.def("title", [](JZGroupBox* w) { return w->title(); });
    bind_group.def("contentWidget", [](JZGroupBox* w) { return w->contentWidget(); }, jsbind::returnOwnedByCpp);    
    m_engine->registClass(bind_group.classDefine());

    // JZSwitch (开关: on/off)
    jsbind::ClassBind<JZSwitch> bind_switch("JZSwitch", bind_w);
    bind_switch.def("setState", [](JZSwitch* w, bool on) { w->setState(on); });
    bind_switch.def("isOn", [](JZSwitch* w) { return w->isOn(); });
    //defSignalProperty(bind_switch, "onToggled", &JZSwitch::sigToggled);   // 回调带 (bool)    
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
    //defSignalProperty(bind_text, "onTextChanged", &JZTextEdit::sigTextChanged);   // (string)    
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

// ---------- 列表 / 表格的 item 对象 ----------
// 之前刻意没开(item 类型没注册成 JS 类, 所有权/失效逻辑也没接线); 现在按"原子对象 + 组合"补齐:
//   * 构造 `new JZTableWidgetItem(text?)` / `new JZListWidgetItem(text?)` —— JS 拥有, GC 时释放;
//   * 交给容器(setItem / addItem / insertItem)时**所有权翻转**(/Transfer/, 与控件进对象树同一套),
//     之后由容器(unique_ptr)负责 delete;
//   * `item(r,c)` / `item(i)` / `currentItem()` 取回的是**借用句柄**(returnOwnedByCpp): 地址稳定到
//     该项被 takeItem / removeRow / removeColumn / clearContents / clear / removeItem 掉为止 ——
//     那些操作之后旧句柄**不能再碰**(与 C++ 的借用指针同一约束); takeItem 系列把所有权交回 JS。
//   * `data` / `setData` 直接收 JS 值(走注册的 JZJSValuePtr metatype), 不做有损转换。

// JS 值 -> JZVariant: 原样持有。形参是借用的 JSValue, 所以先 dup 再交给 makeValuePtr(它接管所有权)。
static JZVariant jsHeldVariant(JZJSEngine* engine, JSValueConst v)
{
    JSContext* ctx = engine->context();
    return JZVariant::fromValue(engine->makeValuePtr(JS_DupValue(ctx, v)));
}

// 取 item 实参并把所有权翻给容器;/ 传 null / undefined 时返回 nullptr(容器语义: 清空该格/该行)。
template <class Item>
static Item* jsTakeItemArg(JSContext* ctx, JSValueConst value, const char* what)
{
    if (JS_IsNull(value) || JS_IsUndefined(value))
        return nullptr;

    if (!js_check_class_arg<Item>(ctx, value))
        throw std::runtime_error(std::string(what) + ": 需要 " + typeid(Item).name() + "(或 null)");

    //jzSetCppOwner(value, true);   // /Transfer/: 之后由容器负责 delete
    return fromJSValue<Item*>(ctx, value);
}

void JZJSWidgetModule::registItems()
{
    // 构造实参 (text?): 对应 C++ 的 () 与 (const std::string&) 两个构造
    auto parseTextArg = [](JSContext* c, int argc, JSValueConst* argv, const char* what) -> std::string {
        if (argc <= 0 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0]))
            return std::string();
        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(c);
        if (!engine || !engine->checkType(argv[0], JSType_string))
            throw std::runtime_error(std::string(what) + ": 文本需要字符串");
        return fromJSValue<std::string>(c, argv[0]);
    };

    // ---- JZTableWidgetItem ----
    jsbind::ClassBind<JZTableWidgetItem> bind_titem("JZTableWidgetItem");
    bind_titem.def("text", [](JZTableWidgetItem* it) { return it->text(); });
    bind_titem.def("setText", [](JZTableWidgetItem* it, const std::string& t) { it->setText(t); });
    bind_titem.def("icon", [](JZTableWidgetItem* it) { return it->icon(); });
    bind_titem.def("setIcon", [](JZTableWidgetItem* it, JZImage* img) { it->setIcon(*img); });
    bind_titem.def("data", [this](JZTableWidgetItem* it) -> JSValue {
        return variantToJSValue(m_engine->context(), it->data());
    });
    bind_titem.def("setData", [this](JZTableWidgetItem* it, JSValue v) { it->setData(jsHeldVariant(m_engine, v)); });
    bind_titem.def("row", [](JZTableWidgetItem* it) { return it->row(); });         // 未入表为 -1
    bind_titem.def("column", [](JZTableWidgetItem* it) { return it->column(); });   // 未入表为 -1
    bind_titem.def("table", [this](JZTableWidgetItem* it) -> JSValue {
        return m_engine->toJsValueFromPtr(it->table(), true);
    });
    m_engine->registClass(bind_titem.classDefine());

    // ---- JZListWidgetItem ----
    jsbind::ClassBind<JZListWidgetItem> bind_litem("JZListWidgetItem");
    bind_litem.def("text", [](JZListWidgetItem* it) { return it->text(); });
    bind_litem.def("setText", [](JZListWidgetItem* it, const std::string& t) { it->setText(t); });
    bind_litem.def("icon", [](JZListWidgetItem* it) { return it->icon(); });
    bind_litem.def("setIcon", [](JZListWidgetItem* it, JZImage* img) { it->setIcon(*img); });
    bind_litem.def("data", [this](JZListWidgetItem* it) -> JSValue {
        return variantToJSValue(m_engine->context(), it->data());
    });
    bind_litem.def("setData", [this](JZListWidgetItem* it, JSValue v) { it->setData(jsHeldVariant(m_engine, v)); });
    bind_litem.def("listWidget", [this](JZListWidgetItem* it) -> JSValue {
        return m_engine->toJsValueFromPtr(it, true);
    });
    m_engine->registClass(bind_litem.classDefine());
}

// ---------- 容器 / 视图类控件 ----------
//
// 这些控件的数据/子项由 C++ 持有, JS 侧只做"值级"操作(文本、行数、当前项、滚动位置),
// **不从 JS 创建 item 对象** —— 所以也不需要"镜像类"那套东西(原 JZJSListWidget/JZJSTableWidget
// 已删: item 类型根本没注册成 JS 类, 两边的所有权/失效逻辑从未接线)。将来真要开 JS 侧 item 接口,
// 按 JZWidget 那组 js_JZWidget_xxx 的做法写全局 C 函数, 每项的失效开关表放 WidgetInfo::ext。
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
    bind_list.def("insertItem", [this](JZListWidget* l, int i, JSValue v) {
        // Qt 同名同义: 第二实参是字符串就建文本项, 是 JZListWidgetItem 就接管它
        if (JS_IsString(v))
            l->insertItem(i, fromJSValue<std::string>(m_engine->context(), v));
        else
            l->insertItem(i, jsTakeItemArg<JZListWidgetItem>(m_engine->context(), v, "JZListWidget.insertItem"));
    });
    // JZListWidget 与 Qt 一样**没有** removeItem: 摘除 + 释放 = takeItem 后自己 delete
    bind_list.def("removeItem", [](JZListWidget* l, int i) { delete l->takeItem(i); });
    bind_list.def("clear", [](JZListWidget* l) { l->clear(); });
    bind_list.def("count", [](JZListWidget* l) { return l->count(); });
    bind_list.def("itemText", [](JZListWidget* l, int i) { return l->itemText(i); });
    bind_list.def("itemData", [this](JZListWidget* l, int i) -> JSValue {
        return variantToJSValue(m_engine->context(), l->itemData(i));
    });

    // ---- item 对象形态(所有权: 交给列表 = 交给列表 delete; takeItem = 交回 JS) ----
    bind_list.def("addItem", [this](JZListWidget* l, JSValue v) {
        // Qt: addItem(QString) / addItem(QListWidgetItem*)
        if (JS_IsString(v))
            l->appendItem(fromJSValue<std::string>(m_engine->context(), v));
        else
            l->appendItem(jsTakeItemArg<JZListWidgetItem>(m_engine->context(), v, "JZListWidget.addItem"));
    });
    bind_list.def("item", [this](JZListWidget* l, int i) -> JSValue {
        JZListWidgetItem* it = l->item(i);
        return m_engine->toJsValueFromPtr(it, true);
    });
    bind_list.def("takeItem", [this](JZListWidget* l, int i) -> JSValue {
        JZListWidgetItem* it = l->takeItem(i);
        return m_engine->toJsValueFromPtr(it, false);
    });
    bind_list.def("row", [](JZListWidget* l, JZListWidgetItem* it) { return l->row(it); });
    bind_list.def("currentItem", [this](JZListWidget* l) -> JSValue {
        JZListWidgetItem* it = l->currentItem();
        return m_engine->toJsValueFromPtr(it, true);
    });
    bind_list.def("setCurrentItem", [](JZListWidget* l, JZListWidgetItem* it) { l->setCurrentItem(it); });
    bind_list.def("currentIndex", [](JZListWidget* l) { return l->currentIndex(); });
    bind_list.def("setCurrentIndex", [](JZListWidget* l, int i) { l->setCurrentIndex(i); });
    bind_list.def("setItemHeight", [](JZListWidget* l, int h) { l->setItemHeight(h); });
    //defSignalProperty(bind_list, "onCurrentIndexChanged", &JZListWidget::currentIndexChanged);   // (int)
    //defSignalProperty(bind_list, "onActivated", &JZListWidget::sigActivated);                    // (int)    
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

    // ---- item 对象形态(所有权: setItem = 交给表格 delete; takeItem = 交回 JS; item/currentItem = 借用) ----
    // 注: 空单元格/无选中在这里**显式判空返回 null** —— 指针-out 通路会把 nullptr 包成一个空壳对象,
    //     而 Qt 语义是 null(item()/currentItem() 可判空)。借用句柄按 toJsValueFromPtr(..., true) 造,
    //     每次调用都是一份新包装(见 JZJSType.h 的说明), 所以别用 `===` 比实例, 判 null 即可。
    bind_table.def("setItem", [this](JZTableWidget* t, int r, int c, JSValue item) {
        t->setItem(r, c, jsTakeItemArg<JZTableWidgetItem>(m_engine->context(), item, "JZTableWidget.setItem"));
    });
    bind_table.def("item", [this](JZTableWidget* t, int r, int c) -> JSValue {
        JZTableWidgetItem* it = t->item(r, c);
        return m_engine->toJsValueFromPtr(it, true);
    });
    bind_table.def("takeItem", [this](JZTableWidget* t, int r, int c) -> JSValue {
        JZTableWidgetItem* it = t->takeItem(r, c);
        return m_engine->toJsValueFromPtr(it, false);
    });
    bind_table.def("row", [](JZTableWidget* t, JZTableWidgetItem* it) { return t->row(it); });
    bind_table.def("column", [](JZTableWidget* t, JZTableWidgetItem* it) { return t->column(it); });
    bind_table.def("currentItem", [this](JZTableWidget* t) -> JSValue {
        JZTableWidgetItem* it = t->currentItem();
        return m_engine->toJsValueFromPtr(it, true);
    });
    bind_table.def("setCurrentItem", [](JZTableWidget* t, JZTableWidgetItem* it) { t->setCurrentItem(it); });
    //defSignalProperty(bind_table, "onCurrentRowChanged", &JZTableWidget::currentRowChanged);   // (int)    
    m_engine->registClass(bind_table.classDefine());

    // JZStackedWidget —— 页容器(注意: 构造函数**没有**默认实参, 必须自定义工厂)
    jsbind::ClassBind<JZStackedWidget> bind_stack("JZStackedWidget", bind_w);
    bind_stack.def("addWidget", [this](JZStackedWidget* s, JSValue w) {
        // 父控件 = 本容器: 走 takeWidgetFromJs(所有权转移 + 登记保活), 与 JZWindow.addWidget 同一套
        return s->addWidget(takeWidgetFromJs(w));
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
        return t->addTab(takeWidgetFromJs(page), label);
    });
    bind_tab.def("count", [](JZTabWidget* t) { return t->count(); });
    bind_tab.def("tabText", [](JZTabWidget* t, int i) { return t->tabText(i); });
    bind_tab.def("currentIndex", [](JZTabWidget* t) { return t->currentIndex(); });
    bind_tab.def("setCurrentIndex", [](JZTabWidget* t, int i) { t->setCurrentIndex(i); });
    //defSignalProperty(bind_tab, "onCurrentChanged", &JZTabWidget::currentChanged);   // (int) 索引    
    m_engine->registClass(bind_tab.classDefine());

    // JZSplitter —— 可拖分隔容器(方向: 0=Horizontal 1=Vertical, 构造实参被忽略 → 用 setOrientation)
    jsbind::ClassBind<JZSplitter> bind_split("JZSplitter", bind_w);
    bind_split.def("setOrientation", [](JZSplitter* s, int o) { s->setOrientation((JZSplitter::Orientation)o); });
    bind_split.def("orientation", [](JZSplitter* s) { return (int)s->orientation(); });
    bind_split.def("addWidget", [this](JZSplitter* s, JSValue w) {
        s->addWidget(takeWidgetFromJs(w));
    });
    bind_split.def("insertWidget", [this](JZSplitter* s, int i, JSValue w) {
        s->insertWidget(i, takeWidgetFromJs(w));
    });
    bind_split.def("removeWidget", [](JZSplitter* s, JZWidget* w) { s->removeWidget(w); });
    bind_split.def("count", [](JZSplitter* s) { return s->count(); });
    bind_split.def("setStretchFactor", [](JZSplitter* s, int i, int v) { s->setStretchFactor(i, v); });
    bind_split.def("setHandleWidth", [](JZSplitter* s, int w) { s->setHandleWidth(w); });
    bind_split.def("handleWidth", [](JZSplitter* s) { return s->handleWidth(); });
    bind_split.def("setCollapsible", [](JZSplitter* s, int i, bool c) { s->setCollapsible(i, c); });
    //defSignalProperty(bind_split, "onSplitterMoved", &JZSplitter::sigSplitterMoved);   // (pos, handleIndex)    
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
    //defSignalProperty(bind_sbar, "onValueChanged", &JZScrollBar::sigValueChanged);   // (int)    
    m_engine->registClass(bind_sbar.classDefine());

    // JZScrollArea —— 滚动区(setWidget 的内容控件由滚动区接管生命周期)
    jsbind::ClassBind<JZScrollArea> bind_area("JZScrollArea", bind_w);
    // 内容控件由滚动区接管生命周期 ⇒ 形参收 JSValue 把 JS 侧包装翻成 C++ 拥有(/Transfer/)
    // 并登记保活(父控件 = 本滚动区; setWidget 内部会把它挂到内部 viewport 下, 属本控件的对象树)
    bind_area.def("setWidget", [this](JZScrollArea* a, JSValue c) {
        a->setWidget(takeWidgetFromJs(c));
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
        JZWidget* child = takeWidgetFromJs(w);
        l->addWidget(child, row, col);
    });
    bind_grid.def("addWidgetSpan", [this](JZGridLayout* l, JSValue w, int r, int c, int rs, int cs) {
        JZWidget* child = takeWidgetFromJs(w);
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

    auto* cp = (jsbind::JSPointerBase*)opaque;
    if (!cp->getRaw())
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

void JZJSWidgetModule::registPainter()
{
    jsbind::ClassBind<JZPainter> bind_p("JZPainter");
    // 创建请求全部转发给模块持有的 JZJSPaintScope(动态转发 + 登记 + 生命周期):
    //   new JZPainter(widget) -> 屏幕画笔, 等价 C++ 的栈对象 `JZPainter p(this)`, 只在本次 onPaint 内有效
    //   new JZPainter(image)  -> 离屏画笔, 自持图层, 析构(或 p.end())时把内容合成回 image
    // 登记/失效只留包装的**共享状态**(JSRawPointer<JZPainter>::Info), 不需要 setInstanceBind 之类的
    // 创建后钩子: create 阶段本就拿不到 JSValue, 也不必拿到 —— 状态独立于 JS 对象存活,
    // 释放点在 paintEvent 末尾(JZJSPaintScope::Guard → end()), 那里用状态就能完成 delete + 失效。
    // 包装模型: opaque 必须是 JSRawPointer<JZPainter>*(createValue 直接塞进 JS 对象),
    // 直接返回底层指针会被后续按 JSPointerBase* 访问 = UB。详见 JZJSPaintScope.h。
    bind_p.setCreate([this](JZJSEngine* ctx, int argc, JSValue* argv) -> jsbind::JSPointerBase* 
    {
        return this->createPainter(ctx->context(), argc, argv);
    });
    // 显式结束绘制: 离屏模式立即把图层合成回 image(之后 image 才可用), 屏幕模式提前补 Pop。
    bind_p.def("end", [this](JZPainter *p) { p->endDevice(); });
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
}   // namespace

void JZJSWidgetModule::registApp()
{
    JSContext* ctx = m_engine->context();
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

JZWidget *JZJSWidgetModule::takeWidgetFromJs(JSValue w)
{
    JSClassID class_id;
    void* ptr = JS_GetAnyOpaque(w, &class_id);

    jsbind::JSObjectPointer* obj_ptr = (jsbind::JSObjectPointer * )ptr;
    obj_ptr->moveToCpp(w);
    return (JZWidget *)obj_ptr->get();
}

JSValue JZJSWidgetModule::moveWidgetToJs(JZWidget *w)
{
    auto ptr = m_engine->getPointer(w);
    return ptr->moveToJs();
}