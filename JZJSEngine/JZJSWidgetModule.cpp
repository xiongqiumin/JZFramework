#include "JZJSWidgetModule.h"

#include "JZJSBind.h"
#include "JZWindow.h"
#include "JZLayout.h"
#include "JZCanvas.h"
#include "JZPainter.h"
#include "JZPen.h"
#include "JZBrush.h"
#include "JZFont.h"
#include "JZColor.h"
#include "JZMatrix2D.h"

#include <string>

// 颜色名/十六进制解析 (供绘制上下文构造器与测试复用)
static JZColor parseColor(const std::string& s);

// ---------- 包裹对象析构 ----------

JZJSWindow::~JZJSWindow()
{
    delete m_win;   // 画布是窗口树的子控件, 由窗口析构统一回收
}

JZJSCanvas::~JZJSCanvas()
{
    if (m_engine)
    {
        JSContext* ctx = m_engine->context();
        if (!JS_IsUndefined(m_paintCb)) JS_FreeValue(ctx, m_paintCb);
        if (!JS_IsUndefined(m_mouseCb)) JS_FreeValue(ctx, m_mouseCb);
    }
    // m_canvas 由窗口树拥有, 这里不 delete
}

// ---------- 模块生命周期 ----------

JZJSWidgetModule::JZJSWidgetModule() {}
JZJSWidgetModule::~JZJSWidgetModule() { deinitEngine(); }

void JZJSWidgetModule::initEngine(JZJSEngine* engine)
{
    m_engine = engine;
    auto global = m_engine->globalObject();
    auto self = this;

    // 原生类 JZWindow
    jsbind::ClassBind<JZJSWindow> bind_win("JZWindow");
    bind_win.def("createCanvas", [self](JZJSWindow* w, int w_, int h_) -> JSValue {
        return self->winCreateCanvas(w, w_, h_);
    });
    bind_win.def("setTitle", [self](JZJSWindow* w, const std::string& t) { self->winSetTitle(w, t); });
    bind_win.def("show", [self](JZJSWindow* w) { self->winShow(w); });
    bind_win.def("close", [self](JZJSWindow* w) { self->winClose(w); });
    m_engine->registClass(bind_win.classDefine());
    m_typeWindow = m_engine->ctypeidToType(typeid(JZJSWindow).name());

    // 原生类 JZCanvas
    jsbind::ClassBind<JZJSCanvas> bind_cv("JZCanvas");
    bind_cv.def("setOnPaint", [self](JZJSCanvas* c, JSValue cb) { self->cvSetOnPaint(c, cb); });
    bind_cv.def("setOnMouse", [self](JZJSCanvas* c, JSValue cb) { self->cvSetOnMouse(c, cb); });
    bind_cv.def("update", [self](JZJSCanvas* c) { self->cvUpdate(c); });
    bind_cv.def("getSize", [self](JZJSCanvas* c) -> JSValue { return self->cvGetSize(c); });
    m_engine->registClass(bind_cv.classDefine());
    m_typeCanvas = m_engine->ctypeidToType(typeid(JZJSCanvas).name());

    // 工厂函数 JZWindow(w, h, title)
    m_engine->registFunction(global, "JZWindow",
        jsbind::createSelfFunction(this, &JZJSWidgetModule::createWindow));
}

void JZJSWidgetModule::deinitEngine()
{
    m_engine = nullptr;
    m_typeWindow = -1;
    m_typeCanvas = -1;
}

// ---------- 工厂 / 方法 ----------

JSValue JZJSWidgetModule::createWindow(int w, int h, const std::string& title)
{
    JZJSWindow* obj = new JZJSWindow();
    obj->m_win = new JZWindow(w, h, title);
    obj->m_box = new JZVBoxLayout(0, 0);
    obj->m_box->setContentsMargins(0, 0, 0, 0);
    obj->m_win->setLayout(obj->m_box);
    return m_engine->createValue(m_typeWindow, obj);
}

JSValue JZJSWidgetModule::winCreateCanvas(JZJSWindow* win, int w, int h)
{
    JZCanvas* cv = new JZCanvas(win->m_win);
    cv->setFixedSize(w, h);
    win->m_box->addWidget(cv, 1);
    JZJSCanvas* obj = new JZJSCanvas();
    obj->m_engine = m_engine;
    obj->m_canvas = cv;
    return m_engine->createValue(m_typeCanvas, obj);
}

void JZJSWidgetModule::winSetTitle(JZJSWindow* win, const std::string& t)
{
    win->m_win->setTitle(t);
}

void JZJSWidgetModule::winShow(JZJSWindow* win)
{
    win->m_win->show();
}

void JZJSWidgetModule::winClose(JZJSWindow* win)
{
    win->m_win->close();
}

void JZJSWidgetModule::cvSetOnPaint(JZJSCanvas* c, JSValue cb)
{
    JSContext* ctx = m_engine->context();
    if (!JS_IsUndefined(c->m_paintCb)) JS_FreeValue(ctx, c->m_paintCb);
    c->m_paintCb = JS_DupValue(ctx, cb);
    c->m_canvas->setPaintCallback([this, c](JZPainter& p) { invokePaint(c, &p); });
}

void JZJSWidgetModule::cvSetOnMouse(JZJSCanvas* c, JSValue cb)
{
    JSContext* ctx = m_engine->context();
    if (!JS_IsUndefined(c->m_mouseCb)) JS_FreeValue(ctx, c->m_mouseCb);
    c->m_mouseCb = JS_DupValue(ctx, cb);
    c->m_canvas->setMouseCallback([this, c](int x, int y, int btn) { invokeMouse(c, x, y, btn); });
}

void JZJSWidgetModule::cvUpdate(JZJSCanvas* c)
{
    c->m_canvas->update();
}

JSValue JZJSWidgetModule::cvGetSize(JZJSCanvas* c)
{
    JSContext* ctx = m_engine->context();
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "w", JS_NewInt32(ctx, c->m_canvas->width()));
    JS_SetPropertyStr(ctx, o, "h", JS_NewInt32(ctx, c->m_canvas->height()));
    return o;
}

// ---------- 回调派发 ----------

// 构造 JS 侧 onPaint 收到的 ctx 对象: 把 JZPainter 的一族绘制方法挂到普通 JS 对象上。
// JSWidgetClass (类继承 onPaint 写法) 与 invokePaint (回调写法) 共用同一套上下文。
JSValue jzBuildPaintContext(JZJSEngine* engine, JZPainter* p)
{
    JSContext* ctx = engine->context();
    JSValue backend = JS_NewObject(ctx);

    auto reg = [&](const char* name, std::shared_ptr<CFunction> f) {
        engine->registFunction(backend, name, f);
    };

    reg("save",           jsbind::createFunction([p]() { p->save(); }));
    reg("restore",        jsbind::createFunction([p]() { p->restore(); }));
    reg("translate",      jsbind::createFunction([p](double dx, double dy) { p->translate(dx, dy); }));
    reg("rotate",         jsbind::createFunction([p](double deg) { p->rotate(deg); }));
    reg("scale",          jsbind::createFunction([p](double sx, double sy) { p->scale(sx, sy); }));
    reg("resetTransform", jsbind::createFunction([p]() { p->resetTransform(); }));
    reg("setClip",        jsbind::createFunction([p](int x, int y, int w, int h) { p->setClip(JZRect(x, y, w, h)); }));
    reg("setPen",         jsbind::createFunction([p](const std::string& color, int width) { p->setPen(JZPen(parseColor(color), width)); }));
    reg("setBrush",       jsbind::createFunction([p](const std::string& color) { p->setBrush(JZBrush(parseColor(color))); }));
    reg("setFont",        jsbind::createFunction([p](const std::string& family, int size) { p->setFont(JZFont(family, size)); }));
    reg("drawLine",       jsbind::createFunction([p](int x1, int y1, int x2, int y2) { p->drawLine(x1, y1, x2, y2); }));
    reg("drawRect",       jsbind::createFunction([p](int x, int y, int w, int h) { p->drawRect(x, y, w, h); }));
    reg("drawRoundRect",  jsbind::createFunction([p](int x, int y, int w, int h, int r) { p->drawRoundRect(x, y, w, h, r); }));
    reg("fillRect",       jsbind::createFunction([p](int x, int y, int w, int h, const std::string& color) { p->fillRect(x, y, w, h, parseColor(color)); }));
    reg("fillRoundRect",  jsbind::createFunction([p](int x, int y, int w, int h, int r, const std::string& color) { p->fillRoundRect(x, y, w, h, r, parseColor(color)); }));
    reg("drawEllipse",    jsbind::createFunction([p](int x, int y, int w, int h) { p->drawEllipse(x, y, w, h); }));
    reg("fillEllipse",    jsbind::createFunction([p](int x, int y, int w, int h, const std::string& color) { p->fillEllipse(x, y, w, h, parseColor(color)); }));
    reg("drawText",       jsbind::createFunction([p](int x, int y, const std::string& text) { p->drawText(x, y, text.c_str()); }));
    // 兼容占位 (五子棋用不到, 调用不报错)
    reg("drawImage",      jsbind::createFunction([](JSValue, int, int, int, int) {}));
    reg("strokePath",     jsbind::createFunction([](JSValue) {}));
    reg("fillPath",       jsbind::createFunction([](JSValue) {}));
    reg("setTransform",   jsbind::createFunction([p](double a, double b, double c_, double d, double e, double f) {
        JZMatrix2D m;
        m.setMatrix(a, b, c_, d, e, f);
        p->setTransform(m);
    }));

    return backend;
}

void JZJSWidgetModule::invokePaint(JZJSCanvas* c, JZPainter* p)
{
    if (JS_IsUndefined(c->m_paintCb))
        return;
    JSContext* ctx = m_engine->context();
    JSValue backend = jzBuildPaintContext(m_engine, p);
    JSValue args[1] = { backend };
    JSValue ret = JS_Call(ctx, c->m_paintCb, JS_UNDEFINED, 1, args);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, backend);
}

void JZJSWidgetModule::invokeMouse(JZJSCanvas* c, int x, int y, int button)
{
    if (JS_IsUndefined(c->m_mouseCb))
        return;
    JSContext* ctx = m_engine->context();
    JSValue args[3] = {
        JS_NewInt32(ctx, x),
        JS_NewInt32(ctx, y),
        JS_NewInt32(ctx, button)
    };
    JSValue ret = JS_Call(ctx, c->m_mouseCb, JS_UNDEFINED, 3, args);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, args[2]);
}

// ---------- 颜色解析 ----------

static JZColor parseColor(const std::string& s)
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
