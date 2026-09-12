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

    registGeometry();
    registColor();
    registImage();
    registEvents();
    registWidgets();
}

void JZJSWidgetModule::deinitEngine()
{
    m_engine = nullptr;
    m_typeWindow = -1;
    m_typeCanvas = -1;
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

void JZJSWidgetModule::registEvents()
{
    // 所有事件子类共享的基类方法 (jsbind 无 JS 原型继承, 故逐个补上)
    auto defEventCommon = [](auto& b) {
        b.def("type", [](auto* e) { return e->type(); });
        b.def("accept", [](auto* e) { e->accept(); });
        b.def("ignore", [](auto* e) { e->ignore(); });
        b.def("isAccepted", [](auto* e) { return e->isAccepted(); });
    };

    jsbind::ClassBind<JZEvent> bind_event("JZEvent");
    bind_event.setCreate([]() { return new jsbind::ClassPointer<JZEvent>(new JZEvent(JZEvent_none), false); });
    defEventCommon(bind_event);
    m_engine->registClass(bind_event.classDefine());

    jsbind::ClassBind<JZMouseEvent> bind_mouse("JZMouseEvent");
    bind_mouse.setCreate([]() {
        return new jsbind::ClassPointer<JZMouseEvent>(
            new JZMouseEvent(JZMouseEvent::Move, JZMouseEvent::NoButton, JZPoint(), JZPoint()), false);
    });
    defEventCommon(bind_mouse);
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

    jsbind::ClassBind<JZKeyEvent> bind_key("JZKeyEvent");
    defEventCommon(bind_key);
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
    bind_resize.setCreate([]() { return new jsbind::ClassPointer<JZResizeEvent>(new JZResizeEvent(JZSize(), JZSize()), false); });
    defEventCommon(bind_resize);
    bind_resize.def("width", [](JZResizeEvent* e) { return e->size().width(); });
    bind_resize.def("height", [](JZResizeEvent* e) { return e->size().height(); });
    bind_resize.def("oldWidth", [](JZResizeEvent* e) { return e->oldSize().width(); });
    bind_resize.def("oldHeight", [](JZResizeEvent* e) { return e->oldSize().height(); });
    bind_resize.def("size", [](JZResizeEvent* e) { return e->size(); });
    bind_resize.def("oldSize", [](JZResizeEvent* e) { return e->oldSize(); });
    m_engine->registClass(bind_resize.classDefine());

    jsbind::ClassBind<JZImeEvent> bind_ime("JZImeEvent");
    defEventCommon(bind_ime);
    bind_ime.def("flags", [](JZImeEvent* e) { return e->flags(); });
    bind_ime.def("hasFlag", [](JZImeEvent* e, int f) { return e->hasFlag((JZImeEvent::Flag)f); });
    bind_ime.def("compString", [](JZImeEvent* e) { return e->compString(); });
    bind_ime.def("resultString", [](JZImeEvent* e) { return e->resultString(); });
    bind_ime.def("compCursor", [](JZImeEvent* e) { return e->compCursor(); });
    m_engine->registClass(bind_ime.classDefine());

    jsbind::ClassBind<JZPainterEvent> bind_paint("JZPainterEvent");
    defEventCommon(bind_paint);
    bind_paint.def("rect", [](JZPainterEvent* e) { return e->rect(); });
    m_engine->registClass(bind_paint.classDefine());

    jsbind::ClassBind<JZShowEvent> bind_show("JZShowEvent");
    defEventCommon(bind_show);
    m_engine->registClass(bind_show.classDefine());
    jsbind::ClassBind<JZHideEvent> bind_hide("JZHideEvent");
    defEventCommon(bind_hide);
    m_engine->registClass(bind_hide.classDefine());
    jsbind::ClassBind<JZCloseEvent> bind_close("JZCloseEvent");
    defEventCommon(bind_close);
    m_engine->registClass(bind_close.classDefine());
    jsbind::ClassBind<JZInvalidateEvent> bind_inval("JZInvalidateEvent");
    defEventCommon(bind_inval);
    bind_inval.def("rect", [](JZInvalidateEvent* e) { return e->rect; });
    m_engine->registClass(bind_inval.classDefine());

    jsbind::ClassBind<JZTimerEvent> bind_timer("JZTimerEvent");
    bind_timer.setCreate([]() { return new jsbind::ClassPointer<JZTimerEvent>(new JZTimerEvent(0), false); });
    defEventCommon(bind_timer);
    bind_timer.def("id", [](JZTimerEvent* e) { return e->id(); });
    m_engine->registClass(bind_timer.classDefine());
}

// ---------- 控件与布局 ----------

void JZJSWidgetModule::registWidgets()
{
    // 基类 JZWidget
    jsbind::ClassBind<JZWidget> bind_w("JZWidget");
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
    bind_w.def("parentWidget", [](JZWidget* w) { return w->parentWidget(); });
    bind_w.def("setLayout", [](JZWidget* w, JZLayout* l) { w->setLayout(l); });
    bind_w.def("layout", [](JZWidget* w) { return w->layout(); });
    bind_w.def("setFocus", [](JZWidget* w) { w->setFocus(); });
    bind_w.def("hasFocus", [](JZWidget* w) { return w->hasFocus(); });
    bind_w.def("update", [](JZWidget* w) { w->update(); });
    bind_w.def("setTooltip", [](JZWidget* w, const std::string& s) { w->setTooltip(JZString(s.c_str())); });
    bind_w.def("tooltip", [](JZWidget* w) { return w->tooltip().toStdString(); });
    bind_w.def("setStyleSheet", [](JZWidget* w, const std::string& css) { return w->setStyleSheet(css); });
    bind_w.def("styleSheet", [](JZWidget* w) { return w->styleSheet(); });
    m_engine->registClass(bind_w.classDefine());

    // JZLabel
    jsbind::ClassBind<JZLabel> bind_label("JZLabel");
    bind_label.def("setText", &JZLabel::setText);
    bind_label.def("text", &JZLabel::text);
    bind_label.def("setAlign", [](JZLabel* w, int a) { w->setAlign(a); });
    m_engine->registClass(bind_label.classDefine());

    // JZPushButton
    jsbind::ClassBind<JZPushButton> bind_btn("JZPushButton");
    bind_btn.def("setText", &JZPushButton::setText);
    bind_btn.def("text", &JZPushButton::text);
    bind_btn.def("setIcon", [](JZPushButton* w, JZImage* img) { w->setIcon(*img); });
    bind_btn.def("icon", [](JZPushButton* w) { return w->icon(); });
    bind_btn.def("setWidth", [](JZPushButton* w, int v) { w->setWidth(v); });
    bind_btn.def("setHeight", [](JZPushButton* w, int v) { w->setHeight(v); });
    m_engine->registClass(bind_btn.classDefine());

    // JZLineEdit (文本编辑接口在 JZAbstractTextEdit)
    jsbind::ClassBind<JZLineEdit> bind_edit("JZLineEdit");
    bind_edit.def("setText", &JZLineEdit::setText);
    bind_edit.def("text", &JZLineEdit::text);
    bind_edit.def("setPassword", [](JZLineEdit* w, bool on) { w->setPassword(on); });
    bind_edit.def("isPassword", [](JZLineEdit* w) { return w->isPassword(); });
    bind_edit.def("setMaxLength", [](JZLineEdit* w, int n) { w->setMaxLength(n); });
    bind_edit.def("maxLength", [](JZLineEdit* w) { return w->maxLength(); });
    bind_edit.def("setReadOnly", [](JZLineEdit* w, bool on) { w->setReadOnly(on); });
    bind_edit.def("isReadOnly", [](JZLineEdit* w) { return w->isReadOnly(); });
    bind_edit.def("setPlaceholderText", [](JZLineEdit* w, const std::string& s) { w->setPlaceholderText(JZString(s.c_str())); });
    bind_edit.def("placeholderText", [](JZLineEdit* w) { return w->placeholderText(); });
    m_engine->registClass(bind_edit.classDefine());

    // JZCheckBox
    jsbind::ClassBind<JZCheckbox> bind_check("JZCheckbox");
    bind_check.def("setText", &JZCheckbox::setText);
    bind_check.def("text", &JZCheckbox::text);
    bind_check.def("setChecked", [](JZCheckbox* w, bool on) { w->setChecked(on); });
    bind_check.def("isChecked", [](JZCheckbox* w) { return w->isChecked(); });
    m_engine->registClass(bind_check.classDefine());

    // JZRadioButton
    jsbind::ClassBind<JZRadioButton> bind_radio("JZRadioButton");
    bind_radio.def("setText", &JZRadioButton::setText);
    bind_radio.def("text", &JZRadioButton::text);
    bind_radio.def("setChecked", [](JZRadioButton* w, bool on) { w->setChecked(on); });
    bind_radio.def("isChecked", [](JZRadioButton* w) { return w->isChecked(); });
    m_engine->registClass(bind_radio.classDefine());

    // JZSlider
    jsbind::ClassBind<JZSlider> bind_slider("JZSlider");
    bind_slider.def("setRange", [](JZSlider* w, int min, int max) { w->setRange(min, max); });
    bind_slider.def("setValue", [](JZSlider* w, int v) { w->setValue(v); });
    bind_slider.def("value", [](JZSlider* w) { return w->value(); });
    m_engine->registClass(bind_slider.classDefine());

    // JZProgressBar
    jsbind::ClassBind<JZProgressBar> bind_bar("JZProgressBar");
    bind_bar.def("setRange", [](JZProgressBar* w, int min, int max) { w->setRange(min, max); });
    bind_bar.def("setValue", [](JZProgressBar* w, int v) { w->setValue(v); });
    bind_bar.def("value", [](JZProgressBar* w) { return w->value(); });
    m_engine->registClass(bind_bar.classDefine());

    // JZComboBox
    jsbind::ClassBind<JZComboBox> bind_combo("JZComboBox");
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
    jsbind::ClassBind<JZSpinBox> bind_spin("JZSpinBox");
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
    jsbind::ClassBind<JZGroupBox> bind_group("JZGroupBox");
    bind_group.def("setTitle", [](JZGroupBox* w, const std::string& s) { w->setTitle(s); });
    bind_group.def("title", [](JZGroupBox* w) { return w->title(); });
    bind_group.def("contentWidget", [](JZGroupBox* w) { return w->contentWidget(); });
    bind_group.def("setLayout", [](JZGroupBox* w, JZLayout* l) { w->setLayout(l); });
    m_engine->registClass(bind_group.classDefine());

    // 布局基类 (抽象, 用默认构造返回 VBoxLayout 实例)
    jsbind::ClassBind<JZLayout> bind_layout("JZLayout");
    bind_layout.setCreate([]() { return new jsbind::ClassPointer<JZLayout>(new JZVBoxLayout(), false); });
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
