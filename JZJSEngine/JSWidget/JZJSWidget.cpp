#include "JZJSWidget.h"

#include "JZDebug.h"            // LOG_W(构造器替换失败时的告警)
#include "JZJSEngine.h"
#include "JZJSType.h"
#include "JZJSWidgetModule.h"   // JZSWidgetEventFilter(事件转发) / jzBindWidgetInstance 的约定
#include "JZColor.h"
#include "JZPen.h"
#include "JZBrush.h"
#include "JZFont.h"
#include "JZMatrix2D.h"
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

// ============================================================================
// JS 构造器(C 版本) —— 每个类一个, 覆盖 C++ 侧的构造重载
// ============================================================================
// 与"引擎按 createArgs 生成的构造器"的区别(那个只管 new 一个默认对象, 收不到实参):
//   * 按 JS 实参形态分派到不同的 C++ 构造; 报错能用 JS_ThrowTypeError 写准;
//   * 构造完统一走 jzNewWidget / jzNewValue / jzNewLayout —— jzNewWidget 会过 createValue
//     (触发 onCreated 回写 m_jsvalue/m_engine), 并把控件登记进 JZJSWidgetModule 的控件表:
//     **有父控件就同步保活它的 JS 包装, 没有父的建顶层节点(不保活, 生死由 JS 决定)**;
//   * 注册见 jzRegistWidgetJsConstructors(在 JZJSWidgetModule::initEngine 末尾调用)。

namespace
{

// ---- 构造参数工具箱(口径统一: 类型不符就抛 TypeError, 返回 false) ----

bool jzIsNullish(JSValueConst v)
{
    return JS_IsNull(v) || JS_IsUndefined(v);
}

// 这个类注册到 JS 了吗(type id); 没注册返回 JSType_none
int jzRegisteredType(JSContext* ctx, const char* ctypeid)
{
    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    return engine ? engine->ctypeidToType(ctypeid) : JSType_none;
}

bool jzIsWidgetArg(JSContext* ctx, JSValueConst v)
{
    return !jzIsNullish(v) && js_check_class_arg<JZWidget>(ctx, v);
}

bool jzIsNumberArg(JSContext* ctx, JSValueConst v)
{
    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    return engine && engine->checkType(v, JSType_int);
}

// 父控件参数: null / undefined / 缺省 = 没有父; 否则必须是控件(或其子类)
bool jzParseParent(JSContext* ctx, JSValueConst v, JZWidget*& parent, const char* what)
{
    if (jzIsNullish(v))
    {
        parent = nullptr;
        return true;
    }
    if (!js_check_class_arg<JZWidget>(ctx, v))
    {
        JS_ThrowTypeError(ctx, "%s: parent 需要 JZWidget(或其子类) 或 null", what);
        return false;
    }
    parent = fromJSValue<JZWidget*>(ctx, v);
    return true;
}

bool jzParseInt(JSContext* ctx, JSValueConst v, int& out, const char* what, int index)
{
    if (!jzIsNumberArg(ctx, v))
    {
        JS_ThrowTypeError(ctx, "%s: 第 %d 个参数需要数字", what, index + 1);
        return false;
    }
    out = fromJSValue<int>(ctx, v);
    return true;
}

bool jzParseString(JSContext* ctx, JSValueConst v, std::string& out, const char* what, int index)
{
    if (!JS_IsString(v))
    {
        JS_ThrowTypeError(ctx, "%s: 第 %d 个参数需要字符串", what, index + 1);
        return false;
    }
    out = fromJSValue<std::string>(ctx, v);
    return true;
}

// 颜色参数: JZColor 对象, 或 '#rrggbb' / '#rgb' / '#rrggbbaa' / 颜色名 字符串
// (与 JZPainter.setPen/setBrush 的解析同一套: jzParseColor)。
bool jzParseColorArg(JSContext* ctx, JSValueConst v, JZColor& out, const char* what)
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

// 数字参数(整数/小数都收)
bool jzParseDouble(JSContext* ctx, JSValueConst v, double& out, const char* what, int index)
{
    if (!JS_IsNumber(v) || JS_ToFloat64(ctx, &out, v) != 0)
    {
        JS_ThrowTypeError(ctx, "%s: 第 %d 个参数需要数字", what, index + 1);
        return false;
    }
    return true;
}

// 0~255 的通道值(opacity 等)
bool jzParseByte(JSContext* ctx, JSValueConst v, uint8_t& out, const char* what, int index)
{
    int n = 0;
    if (!jzParseInt(ctx, v, n, what, index))
        return false;
    if (n < 0 || n > 255)
    {
        JS_ThrowTypeError(ctx, "%s: 第 %d 个参数需要在 0~255 之间", what, index + 1);
        return false;
    }
    out = (uint8_t)n;
    return true;
}

// 至多一个参数: 父控件
bool jzParseParentOnly(JSContext* ctx, int argc, JSValueConst* argv, const char* what, JZWidget*& parent)
{
    if (argc > 1)
    {
        JS_ThrowTypeError(ctx, "%s: 至多 1 个参数(parent)", what);
        return false;
    }
    return argc == 0 ? true : jzParseParent(ctx, argv[0], parent, what);
}

// (text) / (parent) / (text, parent) / (parent, text) —— 文本与父控件顺序无关
bool jzParseTextAndParent(JSContext* ctx, int argc, JSValueConst* argv, const char* what,
                          std::string& text, JZWidget*& parent)
{
    bool hasText = false;
    for (int i = 0; i < argc; i++)
    {
        if (jzIsNullish(argv[i]))
            continue;

        if (jzIsWidgetArg(ctx, argv[i]))
        {
            if (parent)
            {
                JS_ThrowTypeError(ctx, "%s: 父控件只能给一个", what);
                return false;
            }
            parent = fromJSValue<JZWidget*>(ctx, argv[i]);
        }
        else if (JS_IsString(argv[i]))
        {
            if (hasText)
            {
                JS_ThrowTypeError(ctx, "%s: 文本只能给一个", what);
                return false;
            }
            text = fromJSValue<std::string>(ctx, argv[i]);
            hasText = true;
        }
        else
        {
            JS_ThrowTypeError(ctx, "%s: 第 %d 个参数既不是字符串也不是控件", what, i + 1);
            return false;
        }
    }
    return true;
}

// (pad) / (pad, spacing) 这类"最多两个数字"
bool jzParseTwoInts(JSContext* ctx, int argc, JSValueConst* argv, const char* what, int& a, int& b)
{
    if (argc > 2)
    {
        JS_ThrowTypeError(ctx, "%s: 至多两个数字参数", what);
        return false;
    }
    if (argc >= 1 && !jzParseInt(ctx, argv[0], a, what, 0))
        return false;
    if (argc >= 2 && !jzParseInt(ctx, argv[1], b, what, 1))
        return false;
    return true;
}

// ---- 构造收尾 ----

// 控件: 一律 JSObjectPointer(守卫指针) + cppOwner=false(JS 拥有; 挂进对象树时由
// takeWidget/setParentWidget 翻成 C++ 拥有), 包成 JS 对象后登记控件表。
template <class Class>
JSValue jzNewWidget(JSContext* ctx, Class* obj)
{
    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    int type = jzRegisteredType(ctx, typeid(Class).name());
    if (!engine || type == JSType_none)
    {
        delete obj;
        return JS_ThrowInternalError(ctx, "new %s: 类未注册(引擎没装配 JZJSWidgetModule?)", typeid(Class).name());
    }

    JSValue value = engine->createValue(type, new jsbind::JSObjectPointer(obj, false));
    if (JS_IsException(value))
        return value;

    if (JZJSWidgetModule* module = engine->widgetModule())
        module->registerCreatedWidget(obj, value);   // 有父 → 保活; 无父 → 顶层节点

    return value;
}

// 值类型(点/尺寸/矩形/颜色/图片): 一样是指针包装, 但**存一份拷贝** ——
// JS 侧每次拿到独立对象(值语义), 改它不影响别人(与返回值转换的 toJSValue 一致)。
template <class Class>
JSValue jzNewValue(JSContext* ctx, const Class& v)
{
    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    int type = jzRegisteredType(ctx, typeid(Class).name());
    if (!engine || type == JSType_none)
        return JS_ThrowInternalError(ctx, "new %s: 类未注册(引擎没装配 JZJSWidgetModule?)", typeid(Class).name());

    return engine->createValue(type, new jsbind::JSRawPointer<Class>(new Class(v), false));
}

// 布局: 不是 JZObject, 不进控件表; 包装 = JSRawPointer<Class> + cppOwner=false(JS 拥有,
// setLayout 时翻成 C++ 拥有)。JS 类仍是具体布局类(instanceof / 原型链照旧)。
template <class Class>
JSValue jzNewLayout(JSContext* ctx, Class* obj)
{
    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    int type = jzRegisteredType(ctx, typeid(Class).name());
    if (!engine || type == JSType_none)
    {
        delete obj;
        return JS_ThrowInternalError(ctx, "new %s: 类未注册(引擎没装配 JZJSWidgetModule?)", typeid(Class).name());
    }
    return engine->createValue(type, new jsbind::JSRawPointer<Class>(obj, false));
}

// 通用控件的 () / (parent)
template <class Class>
JSValue jzCtorParentOnly(JSContext* ctx, int argc, JSValueConst* argv, const char* what)
{
    JZWidget* parent = nullptr;
    if (!jzParseParentOnly(ctx, argc, argv, what, parent))
        return JS_EXCEPTION;
    return jzNewWidget<Class>(ctx, new Class(parent));
}

}   // namespace

// ---------- 控件 ----------

// JZWidget: () / (parent)
JSValue js_JZWidget_ctor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    JZWidget* parent = nullptr;
    if (!jzParseParentOnly(ctx, argc, argv, "new JZWidget(parent?)", parent))
        return JS_EXCEPTION;
    return jzNewWidget<JZWidget>(ctx, new JZWidget(parent));
}

// JZWindow: () / (parent) / (w, h) / (w, h, title) / (w, h, title, parent)
JSValue js_JZWindow_ctor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    const char* what = "new JZWindow(w?, h?, title?, parent?)";

    if (argc == 0)
        return jzNewWidget<JZWindow>(ctx, new JZWindow());   // 尺寸留 0: 首次 show 时按 sizeHint 自适应

    if (jzIsWidgetArg(ctx, argv[0]) || jzIsNullish(argv[0]))
    {
        // (parent) —— parent 允许 null/undefined(Qt 同款: 没有父窗口)
        if (argc > 1)
        {
            JS_ThrowTypeError(ctx, "%s: 给了 parent 之后不该再有别的参数", what);
            return JS_EXCEPTION;
        }
        JZWidget* parent = nullptr;
        if (!jzParseParent(ctx, argv[0], parent, what))
            return JS_EXCEPTION;
        return jzNewWidget<JZWindow>(ctx, new JZWindow(parent));
    }

    if (argc == 1)
    {
        JS_ThrowTypeError(ctx, "%s: 只给一个数字不够(需要宽高两个)", what);
        return JS_EXCEPTION;
    }
    if (argc > 4)
    {
        JS_ThrowTypeError(ctx, "%s: 参数太多", what);
        return JS_EXCEPTION;
    }

    int w = 0, h = 0;
    std::string title = "JZWindow";
    JZWidget* parent = nullptr;
    if (!jzParseInt(ctx, argv[0], w, what, 0) || !jzParseInt(ctx, argv[1], h, what, 1))
        return JS_EXCEPTION;
    if (argc >= 3 && !jzParseString(ctx, argv[2], title, what, 2))
        return JS_EXCEPTION;
    if (argc >= 4 && !jzParseParent(ctx, argv[3], parent, what))
        return JS_EXCEPTION;

    return jzNewWidget<JZWindow>(ctx, new JZWindow(w, h, title, parent));
}

// JZLabel: () / (parent) / (text) / (text, parent)
JSValue js_JZLabel_ctor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    std::string text;
    JZWidget* parent = nullptr;
    if (!jzParseTextAndParent(ctx, argc, argv, "new JZLabel(text?, parent?)", text, parent))
        return JS_EXCEPTION;
    return jzNewWidget<JZLabel>(ctx, new JZLabel(text, parent));
}

// JZPushButton: () / (parent) / (parent, w, h, text)   —— parent 位置允许 null/undefined(Qt 同款)
JSValue js_JZPushButton_ctor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    const char* what = "new JZPushButton(parent?) / new JZPushButton(parent, w, h, text)";

    if (argc == 0)
        return jzNewWidget<JZPushButton>(ctx, new JZPushButton(nullptr));

    if (!jzIsWidgetArg(ctx, argv[0]) && !jzIsNullish(argv[0]))
    {
        JS_ThrowTypeError(ctx, "%s: 第 1 个参数需要 JZWidget(或其子类) 或 null", what);
        return JS_EXCEPTION;
    }

    JZWidget* parent = nullptr;
    if (!jzParseParent(ctx, argv[0], parent, what))
        return JS_EXCEPTION;
    if (argc == 1)
        return jzNewWidget<JZPushButton>(ctx, new JZPushButton(parent));
    if (argc != 4)
    {
        JS_ThrowTypeError(ctx, "%s: 给成 (parent, w, h, text) 需要 4 个参数", what);
        return JS_EXCEPTION;
    }

    int w = 0, h = 0;
    std::string text;
    if (!jzParseInt(ctx, argv[1], w, what, 1) || !jzParseInt(ctx, argv[2], h, what, 2)
        || !jzParseString(ctx, argv[3], text, what, 3))
        return JS_EXCEPTION;

    return jzNewWidget<JZPushButton>(ctx, new JZPushButton(parent, w, h, text));
}

// () / (parent) 的控件们(构造只有一个 parent 参数)
#define JZ_WIDGET_CTOR_PARENT_ONLY(Fn, Class, Name)                                        \
    JSValue Fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)                 \
    {                                                                                      \
        return jzCtorParentOnly<Class>(ctx, argc, argv, "new " Name "(parent?)");           \
    }

JZ_WIDGET_CTOR_PARENT_ONLY(js_JZLineEdit_ctor,    JZLineEdit,    "JZLineEdit")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZTextEdit_ctor,    JZTextEdit,    "JZTextEdit")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZCheckbox_ctor,    JZCheckbox,    "JZCheckbox")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZSwitch_ctor,      JZSwitch,      "JZSwitch")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZSlider_ctor,      JZSlider,      "JZSlider")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZProgressBar_ctor, JZProgressBar, "JZProgressBar")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZComboBox_ctor,    JZComboBox,    "JZComboBox")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZSpinBox_ctor,     JZSpinBox,     "JZSpinBox")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZChart_ctor,       JZChart,       "JZChart")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZImageLabel_ctor,  JZImageLabel,  "JZImageLabel")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZListWidget_ctor,  JZListWidget,  "JZListWidget")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZTableWidget_ctor, JZTableWidget, "JZTableWidget")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZTabWidget_ctor,   JZTabWidget,   "JZTabWidget")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZScrollArea_ctor,  JZScrollArea,  "JZScrollArea")

#undef JZ_WIDGET_CTOR_PARENT_ONLY

// JZStackedWidget: 构造函数没有默认实参, 所以只能 () / (parent)(这里不套上面的宏, 语义更明确)
JSValue js_JZStackedWidget_ctor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    JZWidget* parent = nullptr;
    if (!jzParseParentOnly(ctx, argc, argv, "new JZStackedWidget(parent?)", parent))
        return JS_EXCEPTION;
    return jzNewWidget<JZStackedWidget>(ctx, new JZStackedWidget(parent));
}

// JZRadioButton: () / (parent) / (parent, text) / (text, parent)
JSValue js_JZRadioButton_ctor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    std::string text;
    JZWidget* parent = nullptr;
    if (!jzParseTextAndParent(ctx, argc, argv, "new JZRadioButton(parentOrText?, textOrParent?)", text, parent))
        return JS_EXCEPTION;
    return jzNewWidget<JZRadioButton>(ctx, new JZRadioButton(parent, text));
}

// JZGroupBox: () / (parent) / (title) / (title, parent)
JSValue js_JZGroupBox_ctor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    std::string title;
    JZWidget* parent = nullptr;
    if (!jzParseTextAndParent(ctx, argc, argv, "new JZGroupBox(title?, parent?)", title, parent))
        return JS_EXCEPTION;
    return jzNewWidget<JZGroupBox>(ctx, new JZGroupBox(title, parent));
}

// JZSplitter / JZScrollBar: () / (parent) / (orientation) / (orientation, parent)   0=横 1=纵
template <class Class>
static JSValue jzCtorOrientation(JSContext* ctx, int argc, JSValueConst* argv, const char* what)
{
    JZWidget* parent = nullptr;
    int orientation = (int)Class::Horizontal;

    if (argc >= 1)
    {
        if (jzIsWidgetArg(ctx, argv[0]))
        {
            // (parent)
            if (argc > 1)
            {
                JS_ThrowTypeError(ctx, "%s: 给了 parent 之后不该再有别的参数", what);
                return JS_EXCEPTION;
            }
            if (!jzParseParent(ctx, argv[0], parent, what))
                return JS_EXCEPTION;
            return jzNewWidget<Class>(ctx, new Class(Class::Horizontal, parent));
        }

        if (!jzParseInt(ctx, argv[0], orientation, what, 0))
            return JS_EXCEPTION;
        if (argc >= 2 && !jzParseParent(ctx, argv[1], parent, what))
            return JS_EXCEPTION;
        if (argc > 2)
        {
            JS_ThrowTypeError(ctx, "%s: 至多 (orientation, parent) 两个参数", what);
            return JS_EXCEPTION;
        }
    }

    if (orientation != (int)Class::Horizontal && orientation != (int)Class::Vertical)
    {
        JS_ThrowTypeError(ctx, "%s: orientation 只能是 0(横) 或 1(纵)", what);
        return JS_EXCEPTION;
    }
    return jzNewWidget<Class>(ctx, new Class((typename Class::Orientation)orientation, parent));
}

JSValue js_JZSplitter_ctor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    return jzCtorOrientation<JZSplitter>(ctx, argc, argv, "new JZSplitter(orientation?, parent?)");
}

JSValue js_JZScrollBar_ctor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    return jzCtorOrientation<JZScrollBar>(ctx, argc, argv, "new JZScrollBar(orientation?, parent?)");
}

// ---------- 布局 ----------

// JZLayout(抽象基类): () / (pad) / (pad, spacing) —— 与文档一致地造一个 JZVBoxLayout,
// JS 类仍是 JZLayout(instanceof JZVBoxLayout 为假)。
JSValue js_JZLayout_ctor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    int pad = 0, spacing = 0;
    if (!jzParseTwoInts(ctx, argc, argv, "new JZLayout(pad?, spacing?)", pad, spacing))
        return JS_EXCEPTION;
    return jzNewLayout<JZLayout>(ctx, (JZLayout*)new JZVBoxLayout(pad, spacing));
}

// JZVBoxLayout / JZHBoxLayout: () / (pad) / (pad, spacing)
template <class Class>
static JSValue jzCtorBoxLayout(JSContext* ctx, int argc, JSValueConst* argv, const char* what)
{
    int pad = 0, spacing = 0;
    if (!jzParseTwoInts(ctx, argc, argv, what, pad, spacing))
        return JS_EXCEPTION;
    return jzNewLayout<Class>(ctx, new Class(pad, spacing));
}

JSValue js_JZVBoxLayout_ctor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    return jzCtorBoxLayout<JZVBoxLayout>(ctx, argc, argv, "new JZVBoxLayout(pad?, spacing?)");
}

JSValue js_JZHBoxLayout_ctor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    return jzCtorBoxLayout<JZHBoxLayout>(ctx, argc, argv, "new JZHBoxLayout(pad?, spacing?)");
}

// JZGridLayout: () / (pad) / (pad, gap)
JSValue js_JZGridLayout_ctor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    int pad = 0, gap = 0;
    if (!jzParseTwoInts(ctx, argc, argv, "new JZGridLayout(pad?, gap?)", pad, gap))
        return JS_EXCEPTION;
    return jzNewLayout<JZGridLayout>(ctx, new JZGridLayout(pad, gap));
}

// JZSelfLayout: () / (pad)
JSValue js_JZSelfLayout_ctor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    const char* what = "new JZSelfLayout(pad?)";
    if (argc > 1)
    {
        JS_ThrowTypeError(ctx, "%s: 至多 1 个参数", what);
        return JS_EXCEPTION;
    }
    int pad = 0;
    if (argc == 1 && !jzParseInt(ctx, argv[0], pad, what, 0))
        return JS_EXCEPTION;
    return jzNewLayout<JZSelfLayout>(ctx, new JZSelfLayout(pad));
}

// ---------- 值类型 ----------

// JZPoint: () / (x, y)
JSValue js_JZPoint_ctor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    int x = 0, y = 0;
    if (!jzParseTwoInts(ctx, argc, argv, "new JZPoint(x?, y?)", x, y))
        return JS_EXCEPTION;
    return jzNewValue<JZPoint>(ctx, JZPoint(x, y));
}

// JZSize: () / (w, h)
JSValue js_JZSize_ctor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    int w = 0, h = 0;
    if (!jzParseTwoInts(ctx, argc, argv, "new JZSize(w?, h?)", w, h))
        return JS_EXCEPTION;
    return jzNewValue<JZSize>(ctx, JZSize(w, h));
}

// JZRect: () / (x, y) / (x, y, w, h)
JSValue js_JZRect_ctor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    const char* what = "new JZRect() / (x, y) / (x, y, w, h)";
    if (argc != 0 && argc != 2 && argc != 4)
    {
        JS_ThrowTypeError(ctx, "%s: 参数个数只能是 0 / 2 / 4", what);
        return JS_EXCEPTION;
    }

    int x = 0, y = 0, w = 0, h = 0;
    if (argc >= 1 && !jzParseInt(ctx, argv[0], x, what, 0))
        return JS_EXCEPTION;
    if (argc >= 2 && !jzParseInt(ctx, argv[1], y, what, 1))
        return JS_EXCEPTION;
    if (argc >= 3 && !jzParseInt(ctx, argv[2], w, what, 2))
        return JS_EXCEPTION;
    if (argc >= 4 && !jzParseInt(ctx, argv[3], h, what, 3))
        return JS_EXCEPTION;

    return jzNewValue<JZRect>(ctx, JZRect(x, y, w, h));
}

// JZColor: () / (colorEnum) / (r, g, b) / (r, g, b, a)
JSValue js_JZColor_ctor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    const char* what = "new JZColor() / (colorEnum) / (r, g, b) / (r, g, b, a)";

    if (argc == 0)
        return jzNewValue<JZColor>(ctx, JZColor());

    if (argc == 1)
    {
        int e = 0;
        if (!jzParseInt(ctx, argv[0], e, what, 0))
            return JS_EXCEPTION;
        return jzNewValue<JZColor>(ctx, JZColor((JZColor::ColorEnum)e));
    }

    if (argc != 3 && argc != 4)
    {
        JS_ThrowTypeError(ctx, "%s: 参数个数只能是 0 / 1 / 3 / 4", what);
        return JS_EXCEPTION;
    }

    int r = 0, g = 0, b = 0, a = 255;
    if (!jzParseInt(ctx, argv[0], r, what, 0) || !jzParseInt(ctx, argv[1], g, what, 1)
        || !jzParseInt(ctx, argv[2], b, what, 2))
        return JS_EXCEPTION;
    if (argc == 4 && !jzParseInt(ctx, argv[3], a, what, 3))
        return JS_EXCEPTION;

    return jzNewValue<JZColor>(ctx, JZColor((uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a));
}

// JZImage: () / (filePath) / (bytes: Uint8Array)
JSValue js_JZImage_ctor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    const char* what = "new JZImage() / (filePath) / (bytes: Uint8Array)";

    if (argc == 0)
        return jzNewValue<JZImage>(ctx, JZImage());

    if (argc == 1)
    {
        if (JS_IsString(argv[0]))
        {
            std::string file = fromJSValue<std::string>(ctx, argv[0]);
            return jzNewValue<JZImage>(ctx, JZImage(file));
        }

        size_t len = 0;
        uint8_t* buf = JS_GetUint8Array(ctx, &len, argv[0]);
        if (!buf || len == 0)
        {
            JS_ThrowTypeError(ctx, "%s: 需要文件路径字符串或非空 Uint8Array", what);
            return JS_EXCEPTION;
        }
        return jzNewValue<JZImage>(ctx, JZImage((const char*)buf, (int)len));
    }

    JS_ThrowTypeError(ctx, "%s: 至多 1 个参数", what);
    return JS_EXCEPTION;
}

// ---------- 画笔样式(JZPen / JZBrush / JZFont) ----------
//
// 三者都是**值语义**描述(不含任何设备资源), 由 JZPainter.setPen/setBrush/setFont 消费:
//   p.setPen(new JZPen(new JZColor(255, 0, 0), 2));      // 或 new JZPen('#ff0000', 2)
//   p.setBrush(new JZBrush('#339933', 128));             // 128 = 半透明
//   p.setFont(new JZFont('微软雅黑', 16, JZFont.Bold));
// 颜色形参同时接受 JZColor 与 '#rrggbb'/颜色名 字符串(解析见 jzParseColorArg)。

// JZPen: () / (color) / (color, width)
JSValue js_JZPen_ctor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    const char* what = "new JZPen(color?, width?)";
    if (argc > 2)
    {
        JS_ThrowTypeError(ctx, "%s: 至多 2 个参数", what);
        return JS_EXCEPTION;
    }

    JZPen pen;   // 无参 = 默认画笔(黑色, 线宽 1)
    if (argc >= 1)
    {
        JZColor color;
        if (!jzParseColorArg(ctx, argv[0], color, what))
            return JS_EXCEPTION;
        int width = 1;
        if (argc >= 2 && !jzParseInt(ctx, argv[1], width, what, 1))
            return JS_EXCEPTION;
        pen = JZPen(color, width);
    }

    return jzNewValue<JZPen>(ctx, pen);
}

// JZBrush: () / (color) / (color, opacity)
JSValue js_JZBrush_ctor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    const char* what = "new JZBrush(color?, opacity?)";
    if (argc > 2)
    {
        JS_ThrowTypeError(ctx, "%s: 至多 2 个参数", what);
        return JS_EXCEPTION;
    }

    JZBrush brush;   // 无参 = 默认画刷(黑色, 不透明)
    if (argc >= 1)
    {
        JZColor color;
        if (!jzParseColorArg(ctx, argv[0], color, what))
            return JS_EXCEPTION;
        uint8_t opa = JZBrush::kOpaCover;
        if (argc >= 2 && !jzParseByte(ctx, argv[1], opa, what, 1))
            return JS_EXCEPTION;
        brush = JZBrush(color, opa);
    }

    return jzNewValue<JZBrush>(ctx, brush);
}

// JZFont: () / (size) / (family, size) / (family, size, style)
JSValue js_JZFont_ctor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    const char* what = "new JZFont(family?, size?, style?)";
    if (argc > 3)
    {
        JS_ThrowTypeError(ctx, "%s: 至多 3 个参数", what);
        return JS_EXCEPTION;
    }

    JZFont font;   // 无参 = 默认字体(字号 14, 由渲染后端决定字族)
    if (argc >= 1)
    {
        // 单参数 = 字号(与 C++ JZFont(int size) 一致); 两个以上第一个必须是字体族
        if (argc == 1)
        {
            int size = 0;
            if (!jzParseInt(ctx, argv[0], size, what, 0))
                return JS_EXCEPTION;
            font = JZFont(size);
        }
        else
        {
            std::string family;
            int size = 0, style = (int)JZFont::Normal;
            if (!jzParseString(ctx, argv[0], family, what, 0)
                || !jzParseInt(ctx, argv[1], size, what, 1))
                return JS_EXCEPTION;
            if (argc >= 3 && !jzParseInt(ctx, argv[2], style, what, 2))
                return JS_EXCEPTION;
            font = JZFont(family, size, style);
        }
    }

    return jzNewValue<JZFont>(ctx, font);
}

// JZMatrix2D: () / (a, b, c, d, e, f) / (other: JZMatrix2D)
//   无参 = 单位阵; 6 个数 = 2x3 仿射部分(行主序 [a b c d e f], 与 GDI+/Canvas 一致)。
JSValue js_JZMatrix2D_ctor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    const char* what = "new JZMatrix2D() / (a, b, c, d, e, f) / (other)";

    if (argc == 0)
        return jzNewValue<JZMatrix2D>(ctx, JZMatrix2D());   // 单位阵

    if (argc == 1)
    {
        // (other): 拷贝构造
        if (!js_check_class_arg<JZMatrix2D>(ctx, argv[0]))
        {
            JS_ThrowTypeError(ctx, "%s: 1 个参数时只接受另一个 JZMatrix2D", what);
            return JS_EXCEPTION;
        }
        return jzNewValue<JZMatrix2D>(ctx, *fromJSValue<JZMatrix2D*>(ctx, argv[0]));
    }

    if (argc != 6)
    {
        JS_ThrowTypeError(ctx, "%s: 参数个数只能是 0 / 1 / 6", what);
        return JS_EXCEPTION;
    }

    double v[6];
    for (int i = 0; i < 6; i++)
    {
        if (!jzParseDouble(ctx, argv[i], v[i], what, i))
            return JS_EXCEPTION;
    }

    JZMatrix2D m;
    m.setMatrix(v[0], v[1], v[2], v[3], v[4], v[5]);
    return jzNewValue<JZMatrix2D>(ctx, m);
}

// ---------- 注册: 把全局构造器换成上面这些 C 版本 ----------

namespace
{

// 换掉一个类在全局上的构造函数:
//   * 新构造器 = JS_NewCFunction2(JS_CFUNC_constructor) —— new X(...) 时直接调我们的 C 函数
//     (quickjs 语义: 构造函数返回对象就用它当结果, 所以不需要 new_target);
//   * 原型沿用引擎注册时建的那个(instanceof / 继承链不变), 只补 prototype/constructor 双向指针;
//   * 旧构造器上的自有可枚举属性(JZChart.LineChart 这类静态常量)搬过来。
void jzReplaceConstructor(JSContext* ctx, const char* name, const char* ctypeid, JSCFunction* fn)
{
    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    if (!engine)
        return;

    int type = engine->ctypeidToType(ctypeid);
    if (type == JSType_none)
    {
        LOG_W(std::string("JZJSWidgetModule: 构造器替换跳过, 类未注册: ") + name);
        return;
    }

    JSValue global = engine->globalObject();
    JSValue oldCtor = JS_GetPropertyStr(ctx, global, name);

    JSValue ctor = JS_NewCFunction2(ctx, fn, name, 0, JS_CFUNC_constructor, 0);
    JSValue proto = JS_GetClassProto(ctx, engine->typeToJSType(type));
    JS_DefinePropertyValueStr(ctx, ctor, "prototype", JS_DupValue(ctx, proto), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, proto, "constructor", JS_DupValue(ctx, ctor),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_FreeValue(ctx, proto);

    // 搬旧构造器上的自有可枚举属性(静态常量等)
    JSPropertyEnum* props = nullptr;
    uint32_t count = 0;
    if (!JS_IsUndefined(oldCtor) &&
        JS_GetOwnPropertyNames(ctx, &props, &count, oldCtor, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0)
    {
        for (uint32_t i = 0; i < count; i++)
        {
            JSValue v = JS_GetProperty(ctx, oldCtor, props[i].atom);
            if (!JS_IsException(v))
                JS_SetProperty(ctx, ctor, props[i].atom, v);   // 消费 v
        }
        JS_FreePropertyEnum(ctx, props, count);
    }

    engine->setProperty(global, name, ctor);   // 全局名指向新构造器(setProperty 不拷贝, 不能释放 ctor)
    JS_FreeValue(ctx, oldCtor);
}

}   // namespace

void jzRegistWidgetJsConstructors(JSContext* ctx)
{
    // 控件
    jzReplaceConstructor(ctx, "JZWidget",        typeid(JZWidget).name(),        js_JZWidget_ctor);
    jzReplaceConstructor(ctx, "JZWindow",        typeid(JZWindow).name(),        js_JZWindow_ctor);
    jzReplaceConstructor(ctx, "JZLabel",         typeid(JZLabel).name(),         js_JZLabel_ctor);
    jzReplaceConstructor(ctx, "JZPushButton",    typeid(JZPushButton).name(),    js_JZPushButton_ctor);
    jzReplaceConstructor(ctx, "JZLineEdit",      typeid(JZLineEdit).name(),      js_JZLineEdit_ctor);
    jzReplaceConstructor(ctx, "JZTextEdit",      typeid(JZTextEdit).name(),      js_JZTextEdit_ctor);
    jzReplaceConstructor(ctx, "JZCheckbox",      typeid(JZCheckbox).name(),      js_JZCheckbox_ctor);
    jzReplaceConstructor(ctx, "JZRadioButton",   typeid(JZRadioButton).name(),   js_JZRadioButton_ctor);
    jzReplaceConstructor(ctx, "JZSwitch",        typeid(JZSwitch).name(),        js_JZSwitch_ctor);
    jzReplaceConstructor(ctx, "JZSlider",        typeid(JZSlider).name(),        js_JZSlider_ctor);
    jzReplaceConstructor(ctx, "JZProgressBar",   typeid(JZProgressBar).name(),   js_JZProgressBar_ctor);
    jzReplaceConstructor(ctx, "JZComboBox",      typeid(JZComboBox).name(),      js_JZComboBox_ctor);
    jzReplaceConstructor(ctx, "JZSpinBox",       typeid(JZSpinBox).name(),       js_JZSpinBox_ctor);
    jzReplaceConstructor(ctx, "JZGroupBox",      typeid(JZGroupBox).name(),      js_JZGroupBox_ctor);
    jzReplaceConstructor(ctx, "JZChart",         typeid(JZChart).name(),         js_JZChart_ctor);
    jzReplaceConstructor(ctx, "JZImageLabel",    typeid(JZImageLabel).name(),    js_JZImageLabel_ctor);
    jzReplaceConstructor(ctx, "JZListWidget",    typeid(JZListWidget).name(),    js_JZListWidget_ctor);
    jzReplaceConstructor(ctx, "JZTableWidget",   typeid(JZTableWidget).name(),   js_JZTableWidget_ctor);
    jzReplaceConstructor(ctx, "JZStackedWidget", typeid(JZStackedWidget).name(), js_JZStackedWidget_ctor);
    jzReplaceConstructor(ctx, "JZTabWidget",     typeid(JZTabWidget).name(),     js_JZTabWidget_ctor);
    jzReplaceConstructor(ctx, "JZSplitter",      typeid(JZSplitter).name(),      js_JZSplitter_ctor);
    jzReplaceConstructor(ctx, "JZScrollArea",    typeid(JZScrollArea).name(),    js_JZScrollArea_ctor);
    jzReplaceConstructor(ctx, "JZScrollBar",     typeid(JZScrollBar).name(),     js_JZScrollBar_ctor);

    // 布局
    jzReplaceConstructor(ctx, "JZLayout",        typeid(JZLayout).name(),        js_JZLayout_ctor);
    jzReplaceConstructor(ctx, "JZVBoxLayout",    typeid(JZVBoxLayout).name(),    js_JZVBoxLayout_ctor);
    jzReplaceConstructor(ctx, "JZHBoxLayout",    typeid(JZHBoxLayout).name(),    js_JZHBoxLayout_ctor);
    jzReplaceConstructor(ctx, "JZGridLayout",    typeid(JZGridLayout).name(),    js_JZGridLayout_ctor);
    jzReplaceConstructor(ctx, "JZSelfLayout",    typeid(JZSelfLayout).name(),    js_JZSelfLayout_ctor);

    // 值类型
    jzReplaceConstructor(ctx, "JZPoint",         typeid(JZPoint).name(),         js_JZPoint_ctor);
    jzReplaceConstructor(ctx, "JZSize",          typeid(JZSize).name(),          js_JZSize_ctor);
    jzReplaceConstructor(ctx, "JZRect",          typeid(JZRect).name(),          js_JZRect_ctor);
    jzReplaceConstructor(ctx, "JZColor",         typeid(JZColor).name(),         js_JZColor_ctor);
    jzReplaceConstructor(ctx, "JZImage",         typeid(JZImage).name(),         js_JZImage_ctor);

    // 画笔样式(值类型)
    jzReplaceConstructor(ctx, "JZPen",           typeid(JZPen).name(),           js_JZPen_ctor);
    jzReplaceConstructor(ctx, "JZBrush",         typeid(JZBrush).name(),         js_JZBrush_ctor);
    jzReplaceConstructor(ctx, "JZFont",          typeid(JZFont).name(),          js_JZFont_ctor);

    // 变换矩阵(值类型)
    jzReplaceConstructor(ctx, "JZMatrix2D",      typeid(JZMatrix2D).name(),      js_JZMatrix2D_ctor);
}
