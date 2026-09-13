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
    // 事件在 event() 调用期间有效(栈对象), 故 toJsValueFromPtr 用 cppOwner=true (C++ 持有),
    // JS 仅持句柄、不 delete, 避免 GC 误删栈/框架托管对象。is-a 与基类方法由 JS 原型链(JZEvent)负责。
    // 各事件类在 registEvents 中按 ClassBind<Sub>(name, bind_event) 挂到 JZEvent 原型上。

    // ---------- 绘制 ----------
    virtual void paintEvent(JZPainterEvent* event) override
    {
        if (!hasJsMethod("onPaint"))
            return Class::paintEvent(event);
        callJs("onPaint", { toJsValueFromPtr(m_engine->context(), event, true) });
    }

    // ---------- 鼠标 ----------
    virtual void mousePressEvent(JZMouseEvent* event) override
    {
        if (!hasJsMethod("onMousePress"))
            return Class::mousePressEvent(event);
        callJs("onMousePress", { toJsValueFromPtr(m_engine->context(), event, true) });
    }
    virtual void mouseReleaseEvent(JZMouseEvent* event) override
    {
        if (!hasJsMethod("onMouseRelease"))
            return Class::mouseReleaseEvent(event);
        callJs("onMouseRelease", { toJsValueFromPtr(m_engine->context(), event, true) });
    }
    virtual void mouseMoveEvent(JZMouseEvent* event) override
    {
        if (!hasJsMethod("onMouseMove"))
            return Class::mouseMoveEvent(event);
        callJs("onMouseMove", { toJsValueFromPtr(m_engine->context(), event, true) });
    }
    virtual void wheelEvent(JZMouseEvent* event) override
    {
        if (!hasJsMethod("onWheel"))
            return Class::wheelEvent(event);
        callJs("onWheel", { toJsValueFromPtr(m_engine->context(), event, true) });
    }

    // ---------- 悬停 / 焦点 ----------
    virtual void enterEvent(JZEvent* event) override
    {
        if (!hasJsMethod("onEnter"))
            return Class::enterEvent(event);
        callJs("onEnter", { toJsValueFromPtr(m_engine->context(), event, true) });
    }
    virtual void leaveEvent(JZEvent* event) override
    {
        if (!hasJsMethod("onLeave"))
            return Class::leaveEvent(event);
        callJs("onLeave", { toJsValueFromPtr(m_engine->context(), event, true) });
    }
    virtual void focusInEvent(JZEvent* event) override
    {
        if (!hasJsMethod("onFocusIn"))
            return Class::focusInEvent(event);
        callJs("onFocusIn", { toJsValueFromPtr(m_engine->context(), event, true) });
    }
    virtual void focusOutEvent(JZEvent* event) override
    {
        if (!hasJsMethod("onFocusOut"))
            return Class::focusOutEvent(event);
        callJs("onFocusOut", { toJsValueFromPtr(m_engine->context(), event, true) });
    }

    // ---------- 显示 / 隐藏 / 尺寸 ----------
    virtual void showEvent(JZShowEvent* event) override
    {
        if (!hasJsMethod("onShow"))
            return Class::showEvent(event);
        callJs("onShow", { toJsValueFromPtr(m_engine->context(), event, true) });
    }
    virtual void hideEvent(JZHideEvent* event) override
    {
        if (!hasJsMethod("onHide"))
            return Class::hideEvent(event);
        callJs("onHide", { toJsValueFromPtr(m_engine->context(), event, true) });
    }
    virtual void resizeEvent(JZResizeEvent* event) override
    {
        if (!hasJsMethod("onResize"))
            return Class::resizeEvent(event);
        callJs("onResize", { toJsValueFromPtr(m_engine->context(), event, true) });
    }

    // ---------- 键盘 / 输入法 ----------
    virtual void keyPressEvent(JZKeyEvent* event) override
    {
        if (!hasJsMethod("onKeyPress"))
            return Class::keyPressEvent(event);
        callJs("onKeyPress", { toJsValueFromPtr(m_engine->context(), event, true) });
    }
    virtual void keyReleaseEvent(JZKeyEvent* event) override
    {
        if (!hasJsMethod("onKeyRelease"))
            return Class::keyReleaseEvent(event);
        callJs("onKeyRelease", { toJsValueFromPtr(m_engine->context(), event, true) });
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

    JZJSEngine* m_engine = nullptr;
    JSValue m_jsvalue = JS_UNDEFINED;
};

// 把 JZWidget 的窗口 / 画布 / 绘制暴露给 JS (事件走类继承 + onXxx 属性写法):
//   const win = new JZWindow(w, h, title);       // 注册类, new 构造
//   const cv  = win.createCanvas(w, h);          // 在窗口布局中创建画布
//   win.onPaint = (e) => { const pt = e.painter(); pt.drawLine(0,0,10,10); };  // 事件转发
//   cv.onMousePress = (e) => { const x = e.localX(), y = e.localY(); };        // 局部逻辑坐标
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
