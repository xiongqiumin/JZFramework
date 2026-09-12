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

// 供 JSWidgetClass 与 JZJSWidgetModule 共用的绘制上下文构造器:
// 把 JZPainter 暴露成 JS 侧 onPaint 收到的 ctx 对象 (save/restore/drawLine ...)。
// 返回的对象由调用方负责 JS_FreeValue。
JSValue jzBuildPaintContext(JZJSEngine* engine, JZPainter* painter);

// 通用 JS 子类包裹层: 逐虚函数转发。
// 每个原生虚函数 (paintEvent / mouseXxxEvent / resizeEvent / ...) 单独重写:
// 先判断 JS 侧 (m_jsvalue) 是否实现了对应方法 (onPaint / onMousePress ...),
// 实现了就用 engine->callMemberFunction 转发给 JS, 否则回退到原生 Class 的默认实现。
template <class Class>
class JSWidgetClass : public Class
{
public:
    // ---------- 绘制 ----------
    virtual void paintEvent(JZPainterEvent* event) override
    {
        if (!hasJsMethod("onPaint"))
            return Class::paintEvent(event);
        JSValue ctx = jzBuildPaintContext(m_engine, event->painter());
        callJs("onPaint", { ctx });
    }

    // ---------- 鼠标 ----------
    virtual void mousePressEvent(JZMouseEvent* event) override
    {
        if (!hasJsMethod("onMousePress"))
            return Class::mousePressEvent(event);
        callJs("onMousePress", mouseArgs(event));
    }
    virtual void mouseReleaseEvent(JZMouseEvent* event) override
    {
        if (!hasJsMethod("onMouseRelease"))
            return Class::mouseReleaseEvent(event);
        callJs("onMouseRelease", mouseArgs(event));
    }
    virtual void mouseMoveEvent(JZMouseEvent* event) override
    {
        if (!hasJsMethod("onMouseMove"))
            return Class::mouseMoveEvent(event);
        callJs("onMouseMove", mouseArgs(event));
    }
    virtual void wheelEvent(JZMouseEvent* event) override
    {
        if (!hasJsMethod("onWheel"))
            return Class::wheelEvent(event);
        JSContext* ctx = m_engine->context();
        callJs("onWheel", {
            JS_NewInt32(ctx, event->localPos().x()),
            JS_NewInt32(ctx, event->localPos().y()),
            JS_NewInt32(ctx, event->wheelDelta())
        });
    }

    // ---------- 悬停 / 焦点 ----------
    virtual void enterEvent(JZEvent* event) override
    {
        if (!hasJsMethod("onEnter"))
            return Class::enterEvent(event);
        callJs("onEnter", {});
    }
    virtual void leaveEvent(JZEvent* event) override
    {
        if (!hasJsMethod("onLeave"))
            return Class::leaveEvent(event);
        callJs("onLeave", {});
    }
    virtual void focusInEvent(JZEvent* event) override
    {
        if (!hasJsMethod("onFocusIn"))
            return Class::focusInEvent(event);
        callJs("onFocusIn", {});
    }
    virtual void focusOutEvent(JZEvent* event) override
    {
        if (!hasJsMethod("onFocusOut"))
            return Class::focusOutEvent(event);
        callJs("onFocusOut", {});
    }

    // ---------- 显示 / 隐藏 / 尺寸 ----------
    virtual void showEvent(JZShowEvent* event) override
    {
        if (!hasJsMethod("onShow"))
            return Class::showEvent(event);
        callJs("onShow", {});
    }
    virtual void hideEvent(JZHideEvent* event) override
    {
        if (!hasJsMethod("onHide"))
            return Class::hideEvent(event);
        callJs("onHide", {});
    }
    virtual void resizeEvent(JZResizeEvent* event) override
    {
        if (!hasJsMethod("onResize"))
            return Class::resizeEvent(event);
        JSContext* ctx = m_engine->context();
        callJs("onResize", {
            JS_NewInt32(ctx, event->size().width()),
            JS_NewInt32(ctx, event->size().height())
        });
    }

    // ---------- 键盘 / 输入法 ----------
    virtual void keyPressEvent(JZKeyEvent* event) override
    {
        if (!hasJsMethod("onKeyPress"))
            return Class::keyPressEvent(event);
        callJs("onKeyPress", keyArgs(event));
    }
    virtual void keyReleaseEvent(JZKeyEvent* event) override
    {
        if (!hasJsMethod("onKeyRelease"))
            return Class::keyReleaseEvent(event);
        callJs("onKeyRelease", keyArgs(event));
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

private:
    std::vector<JSValue> mouseArgs(JZMouseEvent* event)
    {
        JSContext* ctx = m_engine->context();
        return {
            JS_NewInt32(ctx, event->localPos().x()),
            JS_NewInt32(ctx, event->localPos().y()),
            JS_NewInt32(ctx, (int)event->button())
        };
    }
    std::vector<JSValue> keyArgs(JZKeyEvent* event)
    {
        JSContext* ctx = m_engine->context();
        return {
            JS_NewInt32(ctx, (int)event->key()),
            m_engine->toJSValue(event->text()),
            JS_NewInt32(ctx, (int)event->modifiers())
        };
    }
};

// 把 JZWidget 的窗口 / 画布暴露给 JS:
//   const win = JZWindow(w, h, title);          // 工厂函数, 返回窗口句柄
//   const cv  = win.createCanvas(w, h);          // 在窗口布局中创建画布
//   cv.setOnPaint((ctx) => { ... });             // ctx 走 JZPainter 后端
//   cv.setOnMouse((x, y, button) => { ... });    // 局部逻辑坐标
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
    void registWidgets();    // JZWidget 及常用控件 / 布局
};

#endif // !JZ_JS_WIDGET_MODULE_H_
