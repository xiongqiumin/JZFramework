#include "JZJSWidgetModule.h"

#include "JZJSBind.h"
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
                                 JZJSPaintScope* paintScope)
{
    // 经多态基类取底层对象: opaque 的包装类型不一定是 JSRawPointer<W>。
    auto* cp = (jsbind::JSPointerBase*)opaque;
    auto* js = static_cast<JSWidgetClass<W>*>((W*)cp->getRaw());
    js->m_engine = engine;
    js->setPaintScope(paintScope);   // paintEvent 里用它界定 JS 侧画笔的生命周期
    js->m_jsvalue = obj;             // 原始句柄(不 dup): 随 JS 自身 GC, 析构不释放, 避免自引用环
}

// ---------- 模块生命周期 ----------

JZJSWidgetModule::JZJSWidgetModule() {}
JZJSWidgetModule::~JZJSWidgetModule() { deinitEngine(); }

void JZJSWidgetModule::initEngine(JZJSEngine* engine)
{
    m_engine = engine;
    m_paintScope.setEngine(engine);   // 画笔作用域随模块: 先绑定引擎, 再注册各类

    registGeometry();
    registColor();
    registImage();
    registEvents();
    registWidgets();
    registPainter();
    registUiLoader();

    m_typeWindow = m_engine->ctypeidToType(typeid(JZWindow).name());    
}

void JZJSWidgetModule::deinitEngine()
{
    m_paintScope.reset();   // 引擎退出: 回收未结束的画笔并断开引擎引用
    m_engine = nullptr;
    m_typeWindow = -1;    
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

void JZJSWidgetModule::registImage()
{
    jsbind::ClassBind<JZImage> bind_img("JZImage");
    bind_img.def("width", [](JZImage* img) { return img->width(); });
    bind_img.def("height", [](JZImage* img) { return img->height(); });
    bind_img.def("isNull", [](JZImage* img) { return img->isNull(); });
    bind_img.def("save", [](JZImage* img, const std::string& file) { return img->save(file); });
    bind_img.def("scaled", [](JZImage* img, int w, int h) { return img->scaled(w, h); });
    // 静态工厂: JZImage.create(w, h, color)
    bind_img.def("create", &JZImage::create);
    m_engine->registClass(bind_img.classDefine());
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

// ---------- 控件与布局 ----------

void JZJSWidgetModule::registWidgets()
{
    // 基类 JZWidget —— 实际实例为 JSWidgetClass<JZWidget>, 用于 onPaint 等事件转发
    jsbind::ClassBind<JZWidget> bind_w("JZWidget");
    bind_w.setCreate([this](JSContext* ctx, int, JSValueConst*) -> void* {
        return new jsbind::JSRawPointer<JZWidget>(new JSWidgetClass<JZWidget>(), false);
    });
    bind_w.setInstanceBind([this](JSContext* ctx, JSValue obj, void* opaque) {
        jzBindWidgetInstance<JZWidget>(ctx, obj, opaque, m_engine, &m_paintScope);
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
    bind_w.def("setParentWidget", [](JZWidget* w, JZWidget* p) { w->setParentWidget(p); });
    bind_w.def("parentWidget", [](JZWidget* w) { return w->parentWidget(); }, jsbind::returnOwnedByCpp);
    bind_w.def("setLayout", [](JZWidget* w, JZLayout* l) { w->setLayout(l); });
    bind_w.def("layout", [](JZWidget* w) { return w->layout(); }, jsbind::returnOwnedByCpp);
    bind_w.def("setFocus", [](JZWidget* w) { w->setFocus(); });
    bind_w.def("hasFocus", [](JZWidget* w) { return w->hasFocus(); });
    bind_w.def("update", [](JZWidget* w) { w->update(); });
    bind_w.def("setTooltip", [](JZWidget* w, const std::string& s) { w->setTooltip(JZString(s.c_str())); });
    bind_w.def("tooltip", [](JZWidget* w) { return w->tooltip().toStdString(); });
    bind_w.def("setStyleSheet", [](JZWidget* w, const std::string& css) { return w->setStyleSheet(css); });
    bind_w.def("styleSheet", [](JZWidget* w) { return w->styleSheet(); });
    m_engine->registClass(bind_w.classDefine());

    // JZWindow (继承 JZWidget) —— 实际实例为 JSWidgetClass<JZWindow>
    auto makeWindow = [](int w, int h, const std::string& title, JZWidget* parent) {
        auto* win = new JSWidgetClass<JZWindow>(w, h, title, parent);
        win->setLayout(new JZVBoxLayout());
        return win;
    };
    jsbind::ClassBind<JZWindow> bind_win("JZWindow", bind_w);
    bind_win.setCreate([this, makeWindow](JSContext* ctx, int argc, JSValueConst* argv) -> void* {
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
        return new jsbind::JSRawPointer<JZWindow>(makeWindow(w, h, title, parent), false);
    });
    bind_win.setInstanceBind([this](JSContext* ctx, JSValue obj, void* opaque) {
        jzBindWidgetInstance<JZWindow>(ctx, obj, opaque, m_engine, &m_paintScope);
    });
    bind_win.def("setTitle", [](JZWindow* w, const std::string& t) { w->setTitle(t); });
    bind_win.def("title", [](JZWindow* w) { return w->title(); });
    bind_win.def("show", [](JZWindow* w) { w->show(); });
    bind_win.def("close", [](JZWindow* w) { w->close(); });
    bind_win.def("addWidget", [](JZWindow* w, JZWidget* c) {
        JZLayout* lay = w->layout();
        if (!lay) { lay = new JZVBoxLayout(); w->setLayout(lay); }
        static_cast<JZBoxLayout*>(lay)->addWidget(c, 0);
    });
    m_engine->registClass(bind_win.classDefine());    

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
    // 点击信号: 把 sigClicked 转接到 JS 回调 (回调在引擎上下文内同步调用)
    bind_btn.def("onClicked", [this](JZPushButton* w, JSValue cb) {
        JSContext* ctx = m_engine->context();
        JSValue held = JS_DupValue(ctx, cb);
        connect(&w->sigClicked, [this, held]() {
            JSContext* ctx = m_engine->context();
            JSValue ret = JS_Call(ctx, held, JS_UNDEFINED, 0, nullptr);
            JS_FreeValue(ctx, ret);
        });
    });
    m_engine->registClass(bind_btn.classDefine());

    // JZLineEdit (文本编辑接口在 JZAbstractTextEdit)
    jsbind::ClassBind<JZLineEdit> bind_edit("JZLineEdit", bind_w);
    bind_edit.def("setText", &JZLineEdit::setText);
    bind_edit.def("text", &JZLineEdit::text);
    bind_edit.def("setPassword", [](JZLineEdit* w, bool on) { w->setPassword(on); });
    bind_edit.def("isPassword", [](JZLineEdit* w) { return w->isPassword(); });
    bind_edit.def("setMaxLength", [](JZLineEdit* w, int n) { w->setMaxLength(n); });
    bind_edit.def("maxLength", [](JZLineEdit* w) { return w->maxLength(); });
    bind_edit.def("setReadOnly", [](JZLineEdit* w, bool on) { w->setReadOnly(on); });
    bind_edit.def("isReadOnly", [](JZLineEdit* w) { return w->isReadOnly(); });
    bind_edit.def("setPlaceholderText", [](JZLineEdit* w, const std::string& s) { w->setPlaceholderText(s); });
    bind_edit.def("placeholderText", [](JZLineEdit* w) { return w->placeholderText(); });
    m_engine->registClass(bind_edit.classDefine());

    // JZCheckBox
    jsbind::ClassBind<JZCheckbox> bind_check("JZCheckbox", bind_w);
    bind_check.def("setText", &JZCheckbox::setText);
    bind_check.def("text", &JZCheckbox::text);
    bind_check.def("setChecked", [](JZCheckbox* w, bool on) { w->setChecked(on); });
    bind_check.def("isChecked", [](JZCheckbox* w) { return w->isChecked(); });
    m_engine->registClass(bind_check.classDefine());

    // JZRadioButton
    jsbind::ClassBind<JZRadioButton> bind_radio("JZRadioButton", bind_w);
    bind_radio.def("setText", &JZRadioButton::setText);
    bind_radio.def("text", &JZRadioButton::text);
    bind_radio.def("setChecked", [](JZRadioButton* w, bool on) { w->setChecked(on); });
    bind_radio.def("isChecked", [](JZRadioButton* w) { return w->isChecked(); });
    m_engine->registClass(bind_radio.classDefine());

    // JZSlider
    jsbind::ClassBind<JZSlider> bind_slider("JZSlider", bind_w);
    bind_slider.def("setRange", [](JZSlider* w, int min, int max) { w->setRange(min, max); });
    bind_slider.def("setValue", [](JZSlider* w, int v) { w->setValue(v); });
    bind_slider.def("value", [](JZSlider* w) { return w->value(); });
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
    bind_combo.def("addItems", [](JZComboBox* w, const std::vector<std::string>& items) { w->addItems(items); });
    bind_combo.def("removeItem", [](JZComboBox* w, int i) { w->removeItem(i); });
    bind_combo.def("clear", [](JZComboBox* w) { w->clear(); });
    bind_combo.def("currentIndex", [](JZComboBox* w) { return w->currentIndex(); });
    bind_combo.def("setCurrentIndex", [](JZComboBox* w, int i) { w->setCurrentIndex(i); });
    bind_combo.def("currentText", [](JZComboBox* w) { return w->currentText(); });
    bind_combo.def("setCurrentText", [](JZComboBox* w, const std::string& s) { w->setCurrentText(s); });
    bind_combo.def("itemText", [](JZComboBox* w, int i) { return w->itemText(i); });
    bind_combo.def("count", [](JZComboBox* w) { return w->count(); });
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
    m_engine->registClass(bind_spin.classDefine());

    // JZGroupBox
    jsbind::ClassBind<JZGroupBox> bind_group("JZGroupBox", bind_w);
    bind_group.def("setTitle", [](JZGroupBox* w, const std::string& s) { w->setTitle(s); });
    bind_group.def("title", [](JZGroupBox* w) { return w->title(); });
    bind_group.def("contentWidget", [](JZGroupBox* w) { return w->contentWidget(); }, jsbind::returnOwnedByCpp);
    bind_group.def("setLayout", [](JZGroupBox* w, JZLayout* l) { w->setLayout(l); });
    m_engine->registClass(bind_group.classDefine());

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

    // 布局基类 (抽象, 用默认构造返回 VBoxLayout 实例)
    jsbind::ClassBind<JZLayout> bind_layout("JZLayout");
    bind_layout.def("setSpacing", [](JZLayout* l, int g) { l->setSpacing(g); });
    bind_layout.def("spacing", [](JZLayout* l) { return l->spacing(); });
    bind_layout.def("setContentsMargins", [](JZLayout* l, int l_, int t, int r, int b) { l->setContentsMargins(l_, t, r, b); });
    m_engine->registClass(bind_layout.classDefine());

    // 盒子布局 (JZVBoxLayout / JZHBoxLayout) — 必须用具体类型:
    // ClassBind<JZBoxLayout> 因 flexFlow() 纯虚无法默认构造, 故分别注册具体子类。
    auto defBoxLayout = [](auto& bind) {
        bind.def("addWidget", [](JZBoxLayout* l, JZWidget* w, int stretch) { l->addWidget(w, stretch); });
        bind.def("addSpacing", [](JZBoxLayout* l, int s) { l->addSpacing(s); });
        bind.def("addStretch", [](JZBoxLayout* l, int s) { l->addStretch(s); });
        bind.def("insertWidget", [](JZBoxLayout* l, int i, JZWidget* w, int stretch) { l->insertWidget(i, w, stretch); });
        bind.def("removeAt", [](JZBoxLayout* l, int i) { l->removeAt(i); });
        bind.def("removeWidget", [](JZBoxLayout* l, JZWidget* w) { l->removeWidget(w); });
        bind.def("clear", [](JZBoxLayout* l) { l->clear(); });
        bind.def("count", [](JZBoxLayout* l) { return l->count(); });
        bind.def("setSpacing", [](JZBoxLayout* l, int g) { l->setSpacing(g); });
        bind.def("spacing", [](JZBoxLayout* l) { return l->spacing(); });
        bind.def("setContentsMargins", [](JZBoxLayout* l, int l_, int t, int r, int b) { l->setContentsMargins(l_, t, r, b); });
    };
    jsbind::ClassBind<JZVBoxLayout> bind_vbox("JZVBoxLayout");
    defBoxLayout(bind_vbox);
    m_engine->registClass(bind_vbox.classDefine());
    
    jsbind::ClassBind<JZHBoxLayout> bind_hbox("JZHBoxLayout");
    defBoxLayout(bind_hbox);
    m_engine->registClass(bind_hbox.classDefine());

    // JZGridLayout
    jsbind::ClassBind<JZGridLayout> bind_grid("JZGridLayout");
    bind_grid.def("addWidget", [](JZGridLayout* l, JZWidget* w, int row, int col) { l->addWidget(w, row, col); });
    bind_grid.def("addWidgetSpan", [](JZGridLayout* l, JZWidget* w, int r, int c, int rs, int cs) { l->addWidget(w, r, c, rs, cs); });
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
    bind_self.def("addWidget", [](JZSelfLayout* l, JZWidget* w, int anchor, int dx, int dy) { l->addWidget(w, (JZSelfLayout::Anchor)anchor, dx, dy); });
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
    bind_p.setCreate([this](JSContext* ctx, int argc, JSValueConst* argv) -> void* {
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