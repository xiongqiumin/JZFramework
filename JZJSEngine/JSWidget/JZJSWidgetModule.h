#ifndef JZ_JS_WIDGET_MODULE_H_
#define JZ_JS_WIDGET_MODULE_H_

#include <string>
#include <vector>
#include "JZJSEngine.h"
#include "JZColor.h"
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
class JZJSWidgetModule
{
public:
    JZJSWidgetModule();
    ~JZJSWidgetModule();

    void initEngine(JZJSEngine* engine);
    void deinitEngine();

private:
    JZJSEngine* m_engine = nullptr;
    int m_typeWindow = -1;
    int m_typeCanvas = -1;

    // JS 侧画笔的创建分发与生命周期(registPainter 的请求全部转发到这里, paintEvent 的作用域也用它)
    JZJSPaintScope m_paintScope;

    // 分类注册 (initEngine 中统一调用)
    void registGeometry();   // JZPoint / JZSize / JZRect
    void registColor();      // JZColor
    void registImage();      // JZImage
    void registEvents();     // JZEvent 及其子类
    void registWidgets();    // JZWidget / JZWindow / JZCanvas 及常用控件 / 布局
    void registPainter();    // JZPainter
    void registUiLoader();   // JZJSUiLoader + 全局 uiLoader 命名空间
};

#endif // !JZ_JS_WIDGET_MODULE_H_
