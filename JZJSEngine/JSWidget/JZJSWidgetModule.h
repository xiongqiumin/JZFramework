#ifndef JZ_JS_WIDGET_MODULE_H_
#define JZ_JS_WIDGET_MODULE_H_

#include <string>
#include <vector>
#include "JZJSEngine.h"
#include "JZColor.h"
#include "JZSignal.h"
#include "quickjs/quickjs.h"
#include "JZPainterEvent.h"
#include "JZMouseEvent.h"
#include "JZResizeEvent.h"
#include "JZKeyEvent.h"
#include "JZImeEvent.h"
#include "JZShowEvent.h"
#include "JZHideEvent.h"
#include "JZJSPaintScope.h"

class JZJSEngine;
class JZWindow;
class JZWidget;
class JZVBoxLayout;
class JZCanvas;
class JZPainter;

// 通用 JS 子类包裹层: 逐虚函数转发。
// 每个原生虚函数 (paintEvent / mouseXxxEvent / resizeEvent / ...) 单独重写:
// 先判断 JS 侧 (m_jsvalue) 是否实现了对应方法 (onPaint / onMousePress ...),
// 实现了就用 engine->callMemberFunction 转发给 JS, 否则回退到原生 Class 的默认实现。
template <class Class>
class JSWidgetClass : public Class
{
public:
    // 继承基类构造(如 JZWindow(w,h,title) / JZCanvas(parent)),
    // 否则 new JSWidgetClass<Class>(...) 找不到匹配构造函数。
    using Class::Class;

    // 所有虚函数转发统一: 框架派发的事件对象直接包成 JS 已注册的事件类实例转给 JS,
    // 不再为每种事件拼自定义参数对象(原 mouseArgs/keyArgs 已删除)。
    // is-a 与基类方法由 JS 原型链(JZEvent)负责, 各事件类在 registEvents 中按
    // ClassBind<Sub>(name, bind_event) 挂到 JZEvent 原型上。
    //
    // 事件对象的包装: 事件是**栈对象**, 只在本次派发期间有效。所以这里手动建
    // JSRawPointer(借用, cppOwner=true —— JS 不负责释放, 避免 GC 误删栈对象),
    // 派发结束后立刻 **clear()**: 包装的 PointInfo 与该 JS 对象共享, clear 会让 JS 侧
    // 持有的那份一起失效 —— 之后 JS 若还留着事件对象, 访问它抛异常而不是读悬垂内存。
    // (不能用 toJsValueFromPtr: 它内部的临时包装在调用返回后就没了, 之后无从清空。)
    template <class E>
    void callEventJs(const char* name, E* event)
    {
        jsbind::JSRawPointer<E> ref(event, true);
        callJs(name, { toJSValue(m_engine->context(), ref) });
        ref.clear();
    }

    // ---------- 绘制 ----------
    virtual void paintEvent(JZPainterEvent* event) override
    {
        if (!hasJsMethod("onPaint"))
            return Class::paintEvent(event);
        // JS 侧画笔(new JZPainter(this))的生命周期 = 本次 onPaint 派发(对应 C++ 的栈对象
        // `JZPainter p(this)`): 出作用域立即回收, 让绘制流的 Push/Pop 同帧配平。
        // 作用域由模块持有(实例创建时经 setPaintScope 回写), 见 JZJSPaintScope.h。
        JZJSPaintScope::Guard guard(m_paintScope);
        callEventJs("onPaint", event);
    }

    // ---------- 鼠标 ----------
    virtual void mousePressEvent(JZMouseEvent* event) override
    {
        if (!hasJsMethod("onMousePress"))
            return Class::mousePressEvent(event);
        callEventJs("onMousePress", event);
    }
    virtual void mouseReleaseEvent(JZMouseEvent* event) override
    {
        if (!hasJsMethod("onMouseRelease"))
            return Class::mouseReleaseEvent(event);
        callEventJs("onMouseRelease", event);
    }
    virtual void mouseMoveEvent(JZMouseEvent* event) override
    {
        if (!hasJsMethod("onMouseMove"))
            return Class::mouseMoveEvent(event);
        callEventJs("onMouseMove", event);
    }
    virtual void wheelEvent(JZMouseEvent* event) override
    {
        if (!hasJsMethod("onWheel"))
            return Class::wheelEvent(event);
        callEventJs("onWheel", event);
    }

    // ---------- 悬停 / 焦点 ----------
    virtual void enterEvent(JZEvent* event) override
    {
        if (!hasJsMethod("onEnter"))
            return Class::enterEvent(event);
        callEventJs("onEnter", event);
    }
    virtual void leaveEvent(JZEvent* event) override
    {
        if (!hasJsMethod("onLeave"))
            return Class::leaveEvent(event);
        callEventJs("onLeave", event);
    }
    virtual void focusInEvent(JZEvent* event) override
    {
        if (!hasJsMethod("onFocusIn"))
            return Class::focusInEvent(event);
        callEventJs("onFocusIn", event);
    }
    virtual void focusOutEvent(JZEvent* event) override
    {
        if (!hasJsMethod("onFocusOut"))
            return Class::focusOutEvent(event);
        callEventJs("onFocusOut", event);
    }

    // ---------- 显示 / 隐藏 / 尺寸 ----------
    virtual void showEvent(JZShowEvent* event) override
    {
        if (!hasJsMethod("onShow"))
            return Class::showEvent(event);
        callEventJs("onShow", event);
    }
    virtual void hideEvent(JZHideEvent* event) override
    {
        if (!hasJsMethod("onHide"))
            return Class::hideEvent(event);
        callEventJs("onHide", event);
    }
    virtual void resizeEvent(JZResizeEvent* event) override
    {
        if (!hasJsMethod("onResize"))
            return Class::resizeEvent(event);
        callEventJs("onResize", event);
    }

    // ---------- 键盘 / 输入法 ----------
    virtual void keyPressEvent(JZKeyEvent* event) override
    {
        if (!hasJsMethod("onKeyPress"))
            return Class::keyPressEvent(event);
        callEventJs("onKeyPress", event);
    }
    virtual void keyReleaseEvent(JZKeyEvent* event) override
    {
        if (!hasJsMethod("onKeyRelease"))
            return Class::keyReleaseEvent(event);
        callEventJs("onKeyRelease", event);
    }

    // JS 侧 (m_jsvalue) 是否实现名为 name 的方法
    bool hasJsMethod(const char* name) const
    {
        if (JS_IsUndefined(m_jsvalue))
            return false;
        JSContext* ctx = m_engine->context();
        JSValue fn = JS_GetPropertyStr(ctx, m_jsvalue, name);
        bool ok = JS_IsFunction(ctx, fn);
        JS_FreeValue(ctx, fn);
        return ok;
    }

    // 以 m_jsvalue 为 this 调用名为 name 的 JS 成员方法。
    // args 中每个 JSValue 的归属权移交给 makeValuePtr (调用结束即释放), 调用方不要再单独释放。
    void callJs(const char* name, const std::vector<JSValue>& args)
    {
        JSContext* ctx = m_engine->context();
        std::vector<JZJSValuePtr> params;
        params.reserve(args.size());
        for (JSValue a : args)
            params.push_back(m_engine->makeValuePtr(a));
        m_engine->callMemberFunction(name,
            JZJSValuePtr(ctx, JS_DupValue(ctx, m_jsvalue)), params);
    }

    // 绘制作用域(模块持有, 实例创建时经 setPaintScope 回写): paintEvent 用它界定
    // "JS 侧 new 出来的画笔"的生命周期。见 JZJSPaintScope.h。
    void setPaintScope(JZJSPaintScope* scope) { m_paintScope = scope; }

    JZJSEngine* m_engine = nullptr;
    JZJSPaintScope* m_paintScope = nullptr;
    JSValue m_jsvalue = JS_UNDEFINED;
};

// 把 JZWidget 的窗口 / 画布 / 绘制暴露给 JS (事件走类继承 + onXxx 属性写法):
//   const win = new JZWindow(w, h, title);       // 注册类, new 构造
//   const cv  = win.createCanvas(w, h);          // 在窗口布局中创建画布
//   cv.onPaint = (e) => { const p = new JZPainter(cv); p.fillRect(...); };  // e.rect() 给出绘制区
//        画笔与 C++ 一致: 在 onPaint 内自建, 只在本次派发期间有效(派发结束即失效)。
//   cv.onMousePress = (e) => { const x = e.localX(), y = e.localY(); };   // 局部逻辑坐标
//   cv.update();                                  // 触发重绘
//   win.show();
// 测宽与绘制都复用框架现有链路, 窗口存在时 runtime 不退出。
class JZJSWidgetModule : public JZObject
{
public:
    JZJSWidgetModule();
    ~JZJSWidgetModule();

    void initEngine(JZJSEngine* engine);
    void deinitEngine();

    JZJSEngine* engine() const { return m_engine; }

    // ======================================================================
    // 控件 JS 包装的"保活"登记 (C++ 对象树 <-> JS 包装 的循环引用)
    // ======================================================================
    // 问题: 控件挂进对象树后由**父对象**负责 delete(包装被翻成 cppOwner=true, /Transfer/),
    //   但 JS 侧一旦放手(局部变量出作用域 / 引用被覆盖), 包装就会被 GC 收走; 而底层 C++ 对象
    //   还持有包装句柄(JSWidgetClass::m_jsvalue)用于事件转发(onPaint/onMousePress/...) —— 之后
    //   每次都往一个已死的 JS 对象上派发(实测表现为事件静默失效 / 收尾时堆校验报错)。
    // 做法: 子控件进树时, 由**父控件的登记节点**持有一份它的 JS 包装引用(保活);
    //   控件销毁(sigDestroyed)时释放。于是"只要 C++ 对象树还持有控件, 它的 JS 包装就活着"。
    // 顶层控件(没有父控件的, 一般就是窗口)**不保活**: 它的生死由 JS 决定 —— JS 放手 ->
    //   包装析构 -> 窗口销毁 -> 没有窗口了, runtime 退出(见 gomoku/main.js 的"必须留在模块作用域")。
    //
    // 登记入口: takeWidget(布局/窗口加控件时, 见 registWidgets/registLayouts) 与
    //   JZWidget.setParentWidget(quickjs C 函数, 经 func_data 拿到本模块, 见 JZJSWidget.cpp)。
    // ----------------------------------------------------------------------

    // 子控件进对象树: 校验 + 翻包装所有权(/Transfer/) + 登记(保活)。失败抛 std::runtime_error(转 JS 异常)。
    JZWidget* takeWidget(JZWidget* parent, JSValueConst value, const char* what);
    // 把 child 登记到 parent 节点下并保活其 JS 包装(childValue); 已在别处登记时先摘下来(改父)。
    void bindWidgetToParent(JZWidget* parent, JZWidget* child, JSValueConst childValue);
    // 控件脱离父树(setParentWidget(null)): 撤销登记与保活(包装交回 JS 拥有)。
    // 注意: 撤销保活可能让包装立刻析构, 从而 delete 该控件 —— 调用方必须把本调用放在最后。
    void unbindWidget(JZWidget* child);

    // quickjs C 函数的 func_data -> 模块指针(见 jsbind::ClassBind::defData)。
    // 约定: func_data[0] 是"模块指针句柄"(以 JS_NewInt64 存的整数, JS 侧看不到也拿不到)。
    // 取不到时返回 nullptr **并已设 JS 异常**, 调用方直接 `return JS_EXCEPTION`。
    static JZJSWidgetModule* fromFuncData(JSContext* ctx, JSValueConst* func_data);

private:
    // 控件登记节点: 与控件树同构(顶层窗口是根, 挂在它下面的子控件各自保活)。
    struct WidgetInfo
    {
        JZObject* object = nullptr;      // 底层控件裸指针(不拥有; 顶层窗口由它的 JS 包装拥有)
        JSValue jsvalue = JS_UNDEFINED;  // 模块 dup 的一份 JS 包装引用("保活"); 顶层节点恒为空
        std::vector<WidgetInfo> children;
    };

    JZJSEngine* m_engine = nullptr;
    int m_typeWindow = -1;
    int m_typeCanvas = -1;

    // JS 侧画笔的创建分发与生命周期(registPainter 的请求全部转发到这里, paintEvent 的作用域也用它)
    JZJSPaintScope m_paintScope;

    // 转接给 JS 的信号回调(JS 函数值, 全部 dup 持有一份)。
    // 信号槽本身不持有 JSValue 的生命周期, 而回调可能在很久以后才触发 —— 不 dup 会被 GC 收走;
    // 又因为只在引擎存活期间有效, 统一登记后由 deinitEngine() 一次性释放。
    std::vector<JSValue> m_jsCallbacks;

    void onWidgetDestroyed(JZObject *object);

    // ---- WidgetInfo 登记树操作 ----
    // 在 list(含子树)里按底层指针找节点; 找不到返回 nullptr。
    WidgetInfo* findWidgetInfoIn(std::vector<WidgetInfo>& list, JZObject* object);
    // 从 list(含子树)里把 object 的节点**摘下来**(连同子树, 不释放任何 jsvalue), move 到 out。
    bool takeWidgetInfoIn(std::vector<WidgetInfo>& list, JZObject* object, WidgetInfo& out);
    // 新建节点并订阅该控件的 sigDestroyed: 节点一建就订阅, 之后无论它挂在谁下面(顶层/子控件),
    // 控件销毁时都会被摘掉并放掉保活引用。重复订阅无害(onWidgetDestroyed 按指针查, 幂等)。
    WidgetInfo* pushWidgetInfo(std::vector<WidgetInfo>& list, JZObject* object);
    WidgetInfo* findWidgetInfo(JZObject* object);
    // 找 object 的节点; 没有就新建(有父控件则挂到父节点下, 否则作为顶层节点)。
    WidgetInfo* ensureWidgetInfo(JZObject* object);
    // 摘下 object 的节点(不释放)
    bool takeWidgetInfo(JZObject* object, WidgetInfo& out);
    // 释放节点与子树里所有保活引用(不 delete 控件本身)
    void freeWidgetInfo(WidgetInfo& node);
    // 摘除并释放(控件销毁时调用)
    void removeWidgetInfo(JZObject* object);

    // 分类注册 (initEngine 中统一调用)
    void registGeometry();   // JZPoint / JZSize / JZRect
    void registColor();      // JZColor
    void registImage();      // JZImage
    void registEvents();     // JZEvent 及其子类
    void registWidgets();    // JZWidget / JZWindow 及基础控件(Label/Button/Slider/.../Chart)
    void registViews();      // 容器/视图类控件(List/Table/Tab/Splitter/ScrollArea/ScrollBar/...)
    void registLayouts();    // JZLayout 及其四个具体布局
    void registPainter();    // JZPainter
    void registUiLoader();   // JZJSUiLoader + 全局 uiLoader 命名空间

    // ---------- JS 信号回调 ----------

    // 登记一份 JS 回调(dup), 返回值供 connect 的 lambda 捕获; deinitEngine 统一释放。
    JSValue holdJsCallback(JSValue cb);
    // 调用 JS 回调一次。argv 里的值由本函数负责释放(与 JS_Call 的"借用"语义相配)。
    JSValue callJsCallback(JSValue fn, int argc, JSValueConst* argv);

    // 把 C++ 信号接到 JS 回调上(按信号形状重载; JS 侧方法名由各 def() 决定)。
    void bindSignal(JZSigVoid& sig, JSValue cb);
    void bindSignal(JZSigBool& sig, JSValue cb);
    void bindSignal(JZSigInt& sig, JSValue cb);
    void bindSignal(JZSigString& sig, JSValue cb);
    void bindSignal(JZSignal<int, int>& sig, JSValue cb);

    // 给类的**构造器**挂静态函数(JS 侧 `JZImage.create(...)`)。
    // def() 注册的是原型方法, 而 JS 里 `JZImage.create` 走的是函数对象自身的原型链
    // (Function.prototype), 取不到原型上的方法 —— 静态入口必须挂在构造器上
    // (与 JZChart 的类型常量同一处 JS_SetPropertyStr)。
    void defStaticFunction(const CClassDefine& define, const char* name, JSCFunction* fn, int length);

    // 顶层节点(没有父控件的控件, 一般就是窗口); 子控件挂在各自父节点的 children 里
    std::vector<WidgetInfo> m_widgetList;

    // 带 func_data 的 C 函数(setParentWidget)要注入的"模块指针句柄": JS_NewInt64(模块指针)。
    // 指针在 x64 上是 48 位, double(53 位尾数)可精确表示; 且 quickjs 的 float64 **不带引用计数**,
    // GC 的 mark/free 对它都是空操作, 所以可以安全地放进 func_data(JS 侧读不到这份数据)。
    JSValue m_widgetData = JS_UNDEFINED;
};

#endif // !JZ_JS_WIDGET_MODULE_H_
