#include "JZJSWidget.h"

#include "JZJSEngine.h"
#include "JZJSType.h"
#include "JZJSWidgetModule.h"   // JSWidgetClass<W> / jzBindWidgetInstance 的约定
#include "JZLayout.h"
#include "JZWidget.h"
#include "JZWindow.h"
#include "JZLabel.h"
#include "JZPushButton.h"
#include "JZLineEdit.h"
#include "JZTextEdit.h"
#include "JZCheckbox.h"
#include "JZRadioButton.h"
#include "JZSwitch.h"
#include "JZSlider.h"
#include "JZProgressBar.h"
#include "JZComboBox.h"
#include "JZSpinBox.h"
#include "JZGroupBox.h"
#include "JZChart.h"
#include "JZImageLabel.h"
#include "JZListWidget.h"
#include "JZTableWidget.h"
#include "JZStackedWidget.h"
#include "JZTabWidget.h"
#include "JZSplitter.h"
#include "JZScrollArea.h"
#include "JZScrollBar.h"

#include <string>
#include <typeinfo>

// 布局包装的"失效开关"缓存键: 存在 **JS 对象自己**的隐藏属性上(非枚举)。
// 好处是不必在 C++ 侧维护"控件 -> 缓存"的表(那张表要跟着控件生死, 很难不出错),
// 而布局被替换/清空时我们手上正好有 this_val, 拿得到旧缓存并让它失效。
// 同一控件 + 同一布局 ⇒ 返回同一个 JS 对象(引用相等性也保住了)。
static const char* kLayoutRefProp = "__jzLayoutRef";

namespace
{

// this_val -> JZWidget; 不是控件 / 包装已失效时返回 nullptr(调用方回一个 JS 异常)。
// 注意顺序: 先验类再取指针 —— fromJSValue 类型不符或包装已 clear 都会**致命断言**。
JZWidget* widgetFromThis(JSContext* ctx, JSValueConst this_val)
{
    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    if (!engine)
        return nullptr;

    JSClassID class_id = 0;
    void* opaque = JS_GetAnyOpaque(this_val, &class_id);
    if (!opaque || !engine->isJSRawPointerClassId(class_id))
        return nullptr;
    if (!js_check_class_arg<JZWidget>(ctx, this_val))
        return nullptr;

    auto* cp = (jsbind::JSPointerBase*)opaque;
    if (cp->isCleared() || !cp->getRaw())
        return nullptr;

    return fromJSValue<JZWidget*>(ctx, this_val);
}

// 取包装的共享状态(非包装对象返回 nullptr)。
jsbind::JSPointerBase* wrapperOf(JSValueConst value)
{
    JSClassID class_id = 0;
    void* opaque = JS_GetAnyOpaque(value, &class_id);
    return (jsbind::JSPointerBase*)opaque;
}

// 造一个 JS 包装, **类 id 取动态类型**(typeid(*obj)): 这样 JS 侧看到的是具体类 ——
//   * `p instanceof JZLabel` 成立;
//   * `w.layout().addWidget(...)` 可用(JZBoxLayout 的方法挂在 JZVBoxLayout 原型上,
//     若按静态类型 JZLayout 造包装, 子类方法在 JS 里就是"不是函数")。
// 动态类型意外没注册时退回基类(cppOwner=true 的借用包装, 两者都能安全释放)。
JSValue makeDynamicValue(JSContext* ctx, const char* dynCtypeid, const char* baseCtypeid,
                         jsbind::JSPointerBase* wrapper)
{
    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    int type = engine->ctypeidToType(dynCtypeid);
    if (type == JSType_none)
        type = engine->ctypeidToType(baseCtypeid);
    if (type == JSType_none)
    {
        delete wrapper;   // 目标类没注册: 别把包装漏在堆上
        return JS_ThrowInternalError(ctx, "JS 包装: 类型未注册(%s)", baseCtypeid);
    }
    return engine->createValue(type, wrapper);
}

// 控件的借用句柄: 用**守卫指针**包装(与创建时一致) —— 控件被对象树 delete 后守卫自动置空,
// JS 手里那份再被调用会抛异常, 而不是悬垂访问。
JSValue toWidgetRef(JSContext* ctx, JZWidget* w)
{
    return makeDynamicValue(ctx, typeid(*w).name(), typeid(JZWidget).name(),
                            new jsbind::JSObjectPointer(w, true));
}

// 布局的借用视图: 布局不是 JZObject(没有守卫指针) ⇒ JSRawPointer + cppOwner=true(JS 不负责 delete)。
JSValue toLayoutRef(JSContext* ctx, JZLayout* l)
{
    return makeDynamicValue(ctx, typeid(*l).name(), typeid(JZLayout).name(),
                            new jsbind::JSRawPointer<JZLayout>(l, true));
}

// 布局包装缓存: 返回一个**新引用**(调用方负责 free)。没有缓存/取值出错返回 JS_UNDEFINED。
JSValue cachedLayoutRef(JSContext* ctx, JSValueConst this_val)
{
    JSValue ref = JS_GetPropertyStr(ctx, this_val, kLayoutRefProp);
    if (JS_IsException(ref))
    {
        JS_FreeValue(ctx, ref);
        return JS_UNDEFINED;
    }
    return ref;
}

// 记下新的布局包装(JS_DefinePropertyValueStr **消费** value, 且用 JS_PROP_CONFIGURABLE
// 使其成为不可枚举属性 —— 不能在 for-in / Object.keys 里冒出来)。
void cacheLayoutRef(JSContext* ctx, JSValueConst this_val, JSValue wrap)
{
    JS_DefinePropertyValueStr(ctx, this_val, kLayoutRefProp, wrap, JS_PROP_CONFIGURABLE);
}

}   // namespace

void jzSetCppOwner(JSValueConst value, bool cppOwner)
{
    jsbind::JSPointerBase* cp = wrapperOf(value);
    if (cp)
        cp->setCppOwner(cppOwner);
}

// ---------- 父控件 ----------

JSValue js_JZWidget_setParentWidget(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv,
                                    int magic, JSValueConst* func_data)
{
    (void)magic;

    if (argc != 1)
        return JS_ThrowTypeError(ctx, "setParentWidget(parent) 需要 1 个参数(传 null 表示脱离父级)");

    // 模块指针由注册处注入(见 defData)。先取, 免得后面改了对象树才发现没有模块可登记。
    JZJSWidgetModule* module = JZJSWidgetModule::fromFuncData(ctx, func_data);
    if (!module)
        return JS_EXCEPTION;

    JZWidget* self = widgetFromThis(ctx, this_val);
    if (!self)
        return JS_ThrowTypeError(ctx, "setParentWidget: this 不是有效的 JZWidget");

    bool hasParent = !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0]);
    JZWidget* parent = nullptr;
    if (hasParent)
    {
        if (!js_check_class_arg<JZWidget>(ctx, argv[0]))
            return JS_ThrowTypeError(ctx, "setParentWidget(parent): 需要 JZWidget(或其子类)");
        parent = fromJSValue<JZWidget*>(ctx, argv[0]);
    }

    self->setParentWidget(parent);

    // /TransferThis/: 挂进父对象树后由父(对象树)负责 delete ⇒ 翻转**自己**的包装为"非拥有";
    // 脱离对象树则交回 JS 拥有。翻转的是共享 PointInfo, JS 侧那份同步生效。
    // (接收者的包装只能从 this_val 拿 —— 这正是本接口要做成 C 函数的原因。)
    jzSetCppOwner(this_val, hasParent);

    // 模块侧登记: 进树 → 由父控件保活本包装; 脱离 → 撤销保活(包装交回 JS)。
    // **必须是最后一步**: 撤销保活可能让包装立刻析构, 顺带把 self 也 delete 掉 —— 之后不得再碰 self。
    if (hasParent)
        module->bindWidgetToParent(parent, self, this_val);
    else
        module->unbindWidget(self);

    return JS_UNDEFINED;
}

JSValue js_JZWidget_parentWidget(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;

    JZWidget* self = widgetFromThis(ctx, this_val);
    if (!self)
        return JS_ThrowTypeError(ctx, "parentWidget: this 不是有效的 JZWidget");

    JZWidget* p = self->parentWidget();
    if (!p)
        return JS_NULL;   // 真正的 null: 空包装对象是真值, JS 侧判不出"没有父控件"

    return toWidgetRef(ctx, p);   // 借用: 父控件由对象树持有, JS 只持句柄
}

// ---------- 所属窗口 ----------

JSValue js_JZWidget_window(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;

    JZWidget* self = widgetFromThis(ctx, this_val);
    if (!self)
        return JS_ThrowTypeError(ctx, "window: this 不是有效的 JZWidget");

    JZWindow* win = self->window();
    if (!win)
        return JS_NULL;

    return toWidgetRef(ctx, win);   // 借用: 窗口由创建方持有
}

// ---------- 坐标映射 ----------

JSValue js_JZWidget_mapTo(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    if (argc != 2 && argc != 3)
        return JS_ThrowTypeError(ctx, "mapTo(parentOrNull, point) 或 mapTo(parentOrNull, x, y)");

    JZWidget* self = widgetFromThis(ctx, this_val);
    if (!self)
        return JS_ThrowTypeError(ctx, "mapTo: this 不是有效的 JZWidget");

    // parent 为 null: 映射到屏幕坐标(与基类语义一致)
    JZWidget* parent = nullptr;
    if (!JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0]))
    {
        if (!js_check_class_arg<JZWidget>(ctx, argv[0]))
            return JS_ThrowTypeError(ctx, "mapTo(parent): 需要 JZWidget(或其子类) 或 null");
        parent = fromJSValue<JZWidget*>(ctx, argv[0]);
    }

    int px = 0;
    int py = 0;
    if (argc == 3)
    {
        // 构造式入口没有形参类型表 ⇒ 自行校验(fromJSValue 不校验会致命断言)
        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        if (!engine->checkType(argv[1], JSType_int) || !engine->checkType(argv[2], JSType_int))
            return JS_ThrowTypeError(ctx, "mapTo(parent, x, y): x/y 需要数字");
        px = fromJSValue<int>(ctx, argv[1]);
        py = fromJSValue<int>(ctx, argv[2]);
    }
    else
    {
        if (!js_check_class_arg<JZPoint>(ctx, argv[1]))
            return JS_ThrowTypeError(ctx, "mapTo(parent, point): point 需要 JZPoint");
        JZPoint* pt = fromJSValue<JZPoint*>(ctx, argv[1]);
        px = pt->x();
        py = pt->y();
    }

    return toJSValue(ctx, self->mapTo(parent, JZPoint(px, py)));   // 值语义: 返回新的 JS JZPoint
}

// ---------- 布局 ----------

JSValue js_JZWidget_layout(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;

    JZWidget* self = widgetFromThis(ctx, this_val);
    if (!self)
        return JS_ThrowTypeError(ctx, "layout: this 不是有效的 JZWidget");

    JZLayout* cur = self->layout();
    if (!cur)
        return JS_NULL;

    // 缓存里还是"同一个布局、且未失效"的包装 → 直接复用(顺带保住引用相等性)
    JSValue cached = cachedLayoutRef(ctx, this_val);
    if (!JS_IsUndefined(cached))
    {
        auto* cp = wrapperOf(cached);
        if (cp && !cp->isCleared() && cp->getRaw() == (void*)cur)
            return cached;   // cached 就是新引用, 直接交出去
        JS_FreeValue(ctx, cached);
    }

    // 新建非拥有视图(布局由控件持有/删除, cppOwner=true 表示 JS 不负责 delete)并记为缓存
    JSValue wrap = toLayoutRef(ctx, cur);
    cacheLayoutRef(ctx, this_val, JS_DupValue(ctx, wrap));
    return wrap;
}

JSValue js_JZWidget_setLayout(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    if (argc != 1)
        return JS_ThrowTypeError(ctx, "setLayout(layout) 需要 1 个参数");

    JZWidget* self = widgetFromThis(ctx, this_val);
    if (!self)
        return JS_ThrowTypeError(ctx, "setLayout: this 不是有效的 JZWidget");

    // Qt 语义(JZWidget::setLayout): 一个控件只能有一个布局, 且只能设一次。
    // null 与"已有布局"都在**翻转所有权之前**挡掉 —— 否则传进来的布局会被翻成 C++ 拥有,
    // 而 setLayout 又拒绝接管, 那份布局就成了没人释放的孤儿。
    if (JS_IsNull(argv[0]) || JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx, "setLayout(null): 不能把布局设为空; 要清空请用 clearLayout()");
    if (self->layout())
        return JS_ThrowTypeError(ctx, "setLayout(layout): 该控件已有布局(一个控件只能有一个); 要替换请先 clearLayout()");

    // 形参收 JSValue 而不是 JZLayout*: 要能接住 JZVBoxLayout / JZGridLayout 等子类
    // (JSRawPointer<JZLayout>& 那类形参只接受精确类), 并且要拿到包装翻转所有权。
    if (!js_check_class_arg<JZLayout>(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "setLayout(layout): 需要 JZLayout(或其子类)");
    JZLayout* lay = fromJSValue<JZLayout*>(ctx, argv[0]);

    // 旧视图(如果缓存里有)失效: 它指向的布局已经不在(否则上面那句已抛) —— 防御性处理
    JSValue old = cachedLayoutRef(ctx, this_val);
    if (!JS_IsUndefined(old))
    {
        auto* cp = wrapperOf(old);
        if (cp && cp->cppOwner())
            cp->clear();   // 只标记失效, 不 delete(该包装 cppOwner=true, 删除由控件负责)
        JS_FreeValue(ctx, old);
    }

    jzSetCppOwner(argv[0], true);   // /Transfer/: JS 造的布局交给控件, 之后由 C++ delete

    self->setLayout(lay);

    // 缓存新布局的包装: 之后 layout() 返回同一个 JS 对象
    cacheLayoutRef(ctx, this_val, toLayoutRef(ctx, lay));
    return JS_UNDEFINED;
}

JSValue js_JZWidget_clearLayout(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;

    JZWidget* self = widgetFromThis(ctx, this_val);
    if (!self)
        return JS_ThrowTypeError(ctx, "clearLayout: this 不是有效的 JZWidget");

    // 布局马上要被 delete ⇒ 已经发给 JS 的那份视图立刻失效(不能等"下次访问再验证", 那时已悬垂)。
    JSValue cached = cachedLayoutRef(ctx, this_val);
    if (!JS_IsUndefined(cached))
    {
        auto* cp = wrapperOf(cached);
        if (cp)
            cp->clear();   // 只标记失效, 不 delete(该包装 cppOwner=true, 删除由控件负责)
        JS_FreeValue(ctx, cached);
    }

    self->clearLayout();
    return JS_UNDEFINED;
}

// 注册: 上面这些 C 函数由 JZJSWidgetModule::registWidgets() 里的
// jsbind::ClassBind<JZWidget>::def(name, JSCFunction*) 直接挂到 JZWidget 原型上。
