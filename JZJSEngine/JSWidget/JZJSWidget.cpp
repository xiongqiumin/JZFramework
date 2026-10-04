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
#include "JZDoubleSpinBox.h"
#include "JZDateEdit.h"
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


// ============================================================================
// JS 构造器(C 版本) —— 每个类一个, 覆盖 C++ 侧的构造重载
// ============================================================================
// 与"引擎按 create 生成的构造器"的区别(那个只管 new 一个默认对象, 收不到实参):
//   * 按 JS 实参形态分派到不同的 C++ 构造; 报错能用 JS_ThrowTypeError 写准;
//   * 构造完统一走 jzNewWidget / jzNewValue / jzNewLayout —— jzNewWidget 会过 createValue
//     (触发 onCreated 回写 m_void*/m_engine), 并把控件登记进 JZJSWidgetModule 的控件表:
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

// 通用控件的 () / (parent): parent 放最后(与所有控件一致)
template <class Class>
void* jzCtorParentOnly(JZJSEngine *engine, int argc, JSValueConst* argv)
{
    JSContext* ctx = engine->context();
    std::vector<ArgResult> r;
    int g = engine->checkAndSetArgs("JZWidget=null", &r);
    if (g == -1)
        return nullptr;
    JZWidget* parent = static_cast<JZWidget*>(r[0].p);
    return new Class(parent);
}

template <class Class>
void* jzNewLayout(JSContext* ctx, Class* obj)
{
    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    return obj;   // 布局无父: 保持 cppOwner = false(JS 拥有)
}

// 值类型: 拷贝一份堆对象包成 JSPointerBase*(cppOwner = false, GC 时释放)。
template <class Class>
void* jzNewValue(JSContext* ctx, const Class& value)
{
    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    return new Class(value);
}

}   // namespace

// ---------- 控件 ----------

// JZWidget: () / (parent)   —— parent 放最后(与所有控件一致)
void* js_JZWidget_ctor(JZJSEngine *engine, int argc, JSValueConst *argv)
{
    JSContext* ctx = engine->context();
    std::vector<ArgResult> r;
    int g = engine->checkAndSetArgs("JZWidget=null", &r);
    if (g == -1)
        return nullptr;
    JZWidget* parent = static_cast<JZWidget*>(r[0].p);
    return new JZWidget(parent);
}

// JZWindow: () / (parent) / (w, h) / (w, h, title) / (w, h, title, parent)
//   实参形态与 TS 声明(js_wrapper/widget/types/widgets.d.ts)一致:
//   * 不给尺寸(() 与 (parent))时用 C++ 默认尺寸 —— uiLoader 的 __mk 就是 `new JZWindow()`
//     之后再用 setGeometry 落到 XML 里的 geometry, 所以窗口的尺寸必须允许省略;
//   * 给了 w 就必须给 h((w,h) 起), `new JZWindow(100)` 仍然是"参数不匹配"错误。
//   parent 统一放最后(与 JZLabel / JZPushButton 等一致), title/parent 可省略(默认 "" / null)。
void* js_JZWindow_ctor(JZJSEngine *engine, int argc, JSValueConst *argv)
{
    JSContext* ctx = engine->context();
    std::vector<ArgResult> r;
    // 重载组: () | (parent:JZWidget) | (w,h)[,(title)][,(parent:JZWidget)] —— 后两者可省略
    int g = engine->checkAndSetArgs("null|JZWidget|int,int,string=\"\",JZWidget=null", &r);
    if (g == -1)
        return nullptr;
    if (g == 0)
        return new JZWindow();
    if (g == 1)
        return new JZWindow(static_cast<JZWidget*>(r[0].p));
    int w = r[0].i;
    int h = r[1].i;
    std::string title = *r[2].str_ptr;
    JZWidget* parent = static_cast<JZWidget*>(r[3].p);
    return new JZWindow(w, h, title, parent);
}

// JZLabel: (text) / (text, parent)   —— parent 放最后
void* js_JZLabel_ctor(JZJSEngine *engine, int argc, JSValueConst *argv)
{
    JSContext* ctx = engine->context();
    std::vector<ArgResult> r;
    int g = engine->checkAndSetArgs("string=\"\",JZWidget=null", &r);
    if (g == -1)
        return nullptr;
    std::string text = *r[0].str_ptr;
    JZWidget* parent = static_cast<JZWidget*>(r[1].p);
    return new JZLabel(text, parent);
}

// JZPushButton: (text) / (text, parent)   —— 对齐 Qt: text 在前, parent 放最后(构造不再收 w/h)
void* js_JZPushButton_ctor(JZJSEngine *engine, int argc, JSValueConst *argv)
{
    JSContext* ctx = engine->context();
    std::vector<ArgResult> r;
    int g = engine->checkAndSetArgs("string=\"\",JZWidget=null", &r);
    if (g == -1)
        return nullptr;
    std::string text = *r[0].str_ptr;
    JZWidget* parent = static_cast<JZWidget*>(r[1].p);
    return new JZPushButton(text, parent);
}

// () / (parent) 的控件们(构造只有一个 parent 参数)
#define JZ_WIDGET_CTOR_PARENT_ONLY(Fn, Class, Name)                                        \
    void* Fn(JZJSEngine *engine, int argc, JSValueConst *argv)                 \
    {                                                                                      \
        return jzCtorParentOnly<Class>(engine, argc, argv);                                 \
    }

JZ_WIDGET_CTOR_PARENT_ONLY(js_JZLineEdit_ctor,    JZLineEdit,    "JZLineEdit")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZTextEdit_ctor,    JZTextEdit,    "JZTextEdit")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZCheckbox_ctor,    JZCheckbox,    "JZCheckbox")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZSwitch_ctor,      JZSwitch,      "JZSwitch")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZSlider_ctor,      JZSlider,      "JZSlider")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZProgressBar_ctor, JZProgressBar, "JZProgressBar")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZComboBox_ctor,    JZComboBox,    "JZComboBox")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZSpinBox_ctor,     JZSpinBox,     "JZSpinBox")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZDoubleSpinBox_ctor, JZDoubleSpinBox, "JZDoubleSpinBox")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZDateEdit_ctor,     JZDateEdit,    "JZDateEdit")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZChart_ctor,       JZChart,       "JZChart")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZImageLabel_ctor,  JZImageLabel,  "JZImageLabel")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZListWidget_ctor,  JZListWidget,  "JZListWidget")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZTableWidget_ctor, JZTableWidget, "JZTableWidget")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZTabWidget_ctor,   JZTabWidget,   "JZTabWidget")
JZ_WIDGET_CTOR_PARENT_ONLY(js_JZScrollArea_ctor,  JZScrollArea,  "JZScrollArea")

#undef JZ_WIDGET_CTOR_PARENT_ONLY

// JZStackedWidget: () / (parent)   —— parent 放最后
void* js_JZStackedWidget_ctor(JZJSEngine *engine, int argc, JSValueConst *argv)
{
    JSContext* ctx = engine->context();
    std::vector<ArgResult> r;
    int g = engine->checkAndSetArgs("JZWidget=null", &r);
    if (g == -1)
        return nullptr;
    JZWidget* parent = static_cast<JZWidget*>(r[0].p);
    return new JZStackedWidget(parent);
}

// JZRadioButton: (text) / (text, parent)   —— parent 放最后
void* js_JZRadioButton_ctor(JZJSEngine *engine, int argc, JSValueConst *argv)
{
    JSContext* ctx = engine->context();
    std::vector<ArgResult> r;
    int g = engine->checkAndSetArgs("string=\"\",JZWidget=null", &r);
    if (g == -1)
        return nullptr;
    std::string text = *r[0].str_ptr;
    JZWidget* parent = static_cast<JZWidget*>(r[1].p);
    return new JZRadioButton(text, parent);
}

// JZGroupBox: (title) / (title, parent)   —— parent 放最后
void* js_JZGroupBox_ctor(JZJSEngine *engine, int argc, JSValueConst *argv)
{
    JSContext* ctx = engine->context();
    std::vector<ArgResult> r;
    int g = engine->checkAndSetArgs("string=\"\",JZWidget=null", &r);
    if (g == -1)
        return nullptr;
    std::string title = *r[0].str_ptr;
    JZWidget* parent = static_cast<JZWidget*>(r[1].p);
    return new JZGroupBox(title, parent);
}

// JZSplitter / JZScrollBar: (orientation) / (orientation, parent)   0=横 1=纵, parent 放最后
template <class Class>
static void* jzCtorOrientation(JZJSEngine *engine, int argc, JSValueConst *argv, const char* what)
{
    JSContext* ctx = engine->context();
    std::vector<ArgResult> r;
    // orientation 默认 0(横); parent 放最后, 缺省 null
    int g = engine->checkAndSetArgs("int=0,JZWidget=null", &r);
    if (g == -1)
        return nullptr;
    int orientation = r[0].i;
    JZWidget* parent = static_cast<JZWidget*>(r[1].p);
    if (orientation != (int)Class::Horizontal && orientation != (int)Class::Vertical)
    {
        JS_ThrowTypeError(ctx, "%s: orientation 只能是 0(横) 或 1(纵)", what);
        return nullptr;
    }
    return new Class((typename Class::Orientation)orientation, parent);
}

void* js_JZSplitter_ctor(JZJSEngine *engine, int argc, JSValueConst *argv)
{
    return jzCtorOrientation<JZSplitter>(engine, argc, argv, "new JZSplitter(orientation?, parent?)");
}

void* js_JZScrollBar_ctor(JZJSEngine *engine, int argc, JSValueConst *argv)
{
    return jzCtorOrientation<JZScrollBar>(engine, argc, argv, "new JZScrollBar(orientation?, parent?)");
}

// ---------- 布局 ----------
// JZVBoxLayout / JZHBoxLayout: () / (pad) / (pad, spacing)
template <class Class>
static void* jzCtorBoxLayout(JZJSEngine *engine, int argc, JSValueConst *argv, const char* /*what*/)
{
    JSContext* ctx = engine->context();
    std::vector<ArgResult> r;
    int g = engine->checkAndSetArgs("int=0,int=0", &r);
    if (g == -1)
        return nullptr;
    return jzNewLayout<Class>(ctx, new Class(r[0].i, r[1].i));
}

void* js_JZVBoxLayout_ctor(JZJSEngine *engine, int argc, JSValueConst *argv)
{
    return jzCtorBoxLayout<JZVBoxLayout>(engine, argc, argv, "new JZVBoxLayout(pad?, spacing?)");
}

void* js_JZHBoxLayout_ctor(JZJSEngine *engine, int argc, JSValueConst *argv)
{
    return jzCtorBoxLayout<JZHBoxLayout>(engine, argc, argv, "new JZHBoxLayout(pad?, spacing?)");
}

// JZGridLayout: () / (pad) / (pad, gap)
void* js_JZGridLayout_ctor(JZJSEngine *engine, int argc, JSValueConst *argv)
{
    JSContext* ctx = engine->context();
    std::vector<ArgResult> r;
    int g = engine->checkAndSetArgs("int=0,int=0", &r);
    if (g == -1)
        return nullptr;
    return jzNewLayout<JZGridLayout>(ctx, new JZGridLayout(r[0].i, r[1].i));
}

// JZSelfLayout: () / (pad)
void* js_JZSelfLayout_ctor(JZJSEngine *engine, int argc, JSValueConst *argv)
{
    return nullptr;
}

// ---------- 值类型 ----------

// JZPoint: () / (x?, y?)  两个 int 均可省略(省略取 0)
void* js_JZPoint_ctor(JZJSEngine *engine, int argc, JSValueConst *argv)
{
    JSContext* ctx = engine->context();
    std::vector<ArgResult> r;
    if (engine->checkAndSetArgs("int=0,int=0", &r) == -1)
        return nullptr;
    return jzNewValue<JZPoint>(ctx, JZPoint(r[0].i, r[1].i));
}

// JZSize: () / (w, h)
void* js_JZSize_ctor(JZJSEngine *engine, int argc, JSValueConst *argv)
{
    JSContext* ctx = engine->context();
    std::vector<ArgResult> r;
    // 检测 + 取值一步到位: "int=0,int=0" ⇒ 两个 int, 均可省略(省略取默认 0)
    if (engine->checkAndSetArgs("int=0,int=0", &r) == -1)
        return nullptr;
    return jzNewValue<JZSize>(ctx, JZSize(r[0].i, r[1].i));
}

// JZRect: () / (x, y) / (x, y, w, h)
void* js_JZRect_ctor(JZJSEngine *engine, int argc, JSValueConst *argv)
{
    JSContext* ctx = engine->context();
    std::vector<ArgResult> r;
    // 三个重载组: 0 参 / (x,y) / (x,y,w,h)
    int g = engine->checkAndSetArgs("null|int,int,int,int", &r);
    if (g == -1)
        return nullptr;
    if (g == 0)
        return jzNewValue<JZRect>(ctx, JZRect());
    else
        return jzNewValue<JZRect>(ctx, JZRect(r[0].i, r[1].i, r[2].i, r[3].i));
}

// JZColor: () / (colorEnum) / (r, g, b) / (r, g, b, a)
void* js_JZColor_ctor(JZJSEngine *engine, int argc, JSValueConst *argv)
{
    JSContext* ctx = engine->context();
    std::vector<ArgResult> r;
    // 重载组: 0 参 / (colorEnum) / (r,g,b) / (r,g,b,a)
    int g = engine->checkAndSetArgs("null|int|int,int,int|int,int,int,int", &r);
    if (g == -1)
        return nullptr;
    if (g == 0)
        return jzNewValue<JZColor>(ctx, JZColor());
    if (g == 1)
        return jzNewValue<JZColor>(ctx, JZColor((JZColor::ColorEnum)r[0].i));
    if (g == 2)
        return jzNewValue<JZColor>(ctx, JZColor((uint8_t)r[0].i, (uint8_t)r[1].i, (uint8_t)r[2].i));
    return jzNewValue<JZColor>(ctx, JZColor((uint8_t)r[0].i, (uint8_t)r[1].i, (uint8_t)r[2].i, (uint8_t)r[3].i));
}

// JZImage: () / (filePath) / (bytes: Uint8Array)
void* js_JZImage_ctor(JZJSEngine *engine, int argc, JSValueConst *argv)
{
    JSContext* ctx = engine->context();
    if (argc == 0)
        return jzNewValue<JZImage>(ctx, JZImage());

    if (argc == 1)
    {
        // (filePath): 按文件头探测格式解码; (bytes: Uint8Array): 按内存数据解码
        if (engine->checkType(argv[0], JSType_string))
        {
            std::string file = fromJSValue<std::string>(ctx, argv[0]);
            return jzNewValue<JZImage>(ctx, JZImage(file));
        }
        // (bytes: Uint8Array) —— 先按类型确认再取指针: JS_GetUint8Array 对非 typed array 会抛
        // "not a TypedArray"(留下待决异常), 不适合当探测用(见 jzUint8ArrayOrNull 的说明)。
        if (JS_IsObject(argv[0]))
        {
            const int ta = JS_GetTypedArrayType(argv[0]);
            if (ta == JS_TYPED_ARRAY_UINT8 || ta == JS_TYPED_ARRAY_UINT8C)
            {
                size_t size = 0;
                const uint8_t* buf = JS_GetUint8Array(ctx, &size, argv[0]);
                if (size == 0 || !buf)
                    return jzNewValue<JZImage>(ctx, JZImage());
                return jzNewValue<JZImage>(ctx, JZImage((const char*)buf, (int)size));
            }
        }
        JS_ThrowTypeError(ctx, "JZImage: 参数应为 () / (filePath) / (bytes: Uint8Array)");
        return nullptr;
    }

    JS_ThrowTypeError(ctx, "JZImage: 参数应为 () / (filePath) / (bytes)");
    return nullptr;
}

// ---------- 画笔样式(JZPen / JZBrush / JZFont) ----------
//
// 三者都是**值语义**描述(不含任何设备资源), 由 JZPainter.setPen/setBrush/setFont 消费:
//   p.setPen(new JZPen(new JZColor(255, 0, 0), 2));      // 或 new JZPen('#ff0000', 2)
//   p.setBrush(new JZBrush('#339933', 128));             // 128 = 半透明
//   p.setFont(new JZFont('微软雅黑', 16, JZFont.Bold));
// 颜色形参同时接受 JZColor 与 '#rrggbb'/颜色名 字符串(解析见 jzParseColorArg)。

// JZPen: () / (color) / (color, width)
//   颜色形参接受 JZColor 对象或 '#rrggbb'/颜色名 字符串(字符串组用 checkAndSetArgs 收下后再解析)
void* js_JZPen_ctor(JZJSEngine *engine, int argc, JSValueConst *argv)
{
    JSContext* ctx = engine->context();
    std::vector<ArgResult> r;
    // 重载组: () / (color:JZColor) / (color:JZColor,width) / (color:string) / (color:string,width)
    int g = engine->checkAndSetArgs("null|JZColor|JZColor,int|string|string,int", &r);
    if (g == -1)
        return nullptr;
    if (g == 0)
        return jzNewValue<JZPen>(ctx, JZPen());   // 默认画笔(黑色, 线宽 1)
    JZColor color;
    int width = 1;
    if (g == 1 || g == 2)
    {
        color = *static_cast<JZColor*>(r[0].p);
        if (g == 2)
            width = r[1].i;
    }
    else // g == 3 || g == 4 : 字符串组, 收到后再解析
    {
        color = jzParseColor(*r[0].str_ptr);
        if (g == 4)
            width = r[1].i;
    }
    return jzNewValue<JZPen>(ctx, JZPen(color, width));
}

// JZBrush: () / (color) / (color, opacity)
//   颜色形参同 JZPen: JZColor 对象 或 '#rrggbb'/颜色名 字符串
void* js_JZBrush_ctor(JZJSEngine *engine, int argc, JSValueConst *argv)
{
    JSContext* ctx = engine->context();
    std::vector<ArgResult> r;
    // 重载组: () / (color:JZColor) / (color:JZColor,opacity) / (color:string) / (color:string,opacity)
    int g = engine->checkAndSetArgs("null|JZColor|JZColor,int|string|string,int", &r);
    if (g == -1)
        return nullptr;
    if (g == 0)
        return jzNewValue<JZBrush>(ctx, JZBrush());   // 默认画刷(黑色, 不透明)
    JZColor color;
    int opa = (int)JZBrush::kOpaCover;
    if (g == 1 || g == 2)
    {
        color = *static_cast<JZColor*>(r[0].p);
        if (g == 2)
            opa = r[1].i;
    }
    else
    {
        color = jzParseColor(*r[0].str_ptr);
        if (g == 4)
            opa = r[1].i;
    }
    return jzNewValue<JZBrush>(ctx, JZBrush(color, (uint8_t)opa));
}

// JZFont: () / (size) / (family, size) / (family, size, style)
void* js_JZFont_ctor(JZJSEngine *engine, int argc, JSValueConst *argv)
{
    JSContext* ctx = engine->context();
    std::vector<ArgResult> r;
    // 重载组: () / (size) / (family,size) / (family,size,style)
    int g = engine->checkAndSetArgs("null|int|string,int|string,int,int", &r);
    if (g == -1)
        return nullptr;
    if (g == 0)
        return jzNewValue<JZFont>(ctx, JZFont());   // 默认字体(字号 14, 由渲染后端决定字族)
    if (g == 1)
        return jzNewValue<JZFont>(ctx, JZFont(r[0].i));
    int size = r[1].i;
    int style = (int)JZFont::Normal;
    if (g == 3)
        style = r[2].i;
    return jzNewValue<JZFont>(ctx, JZFont(*r[0].str_ptr, size, style));
}

// JZMatrix2D: () / (a, b, c, d, e, f) / (other: JZMatrix2D)
//   无参 = 单位阵; 6 个数 = 2x3 仿射部分(行主序 [a b c d e f], 与 GDI+/Canvas 一致)。
void* js_JZMatrix2D_ctor(JZJSEngine *engine, int argc, JSValueConst *argv)
{
    JSContext* ctx = engine->context();
    std::vector<ArgResult> r;
    // 重载组: 0 参(单位阵) / (other:JZMatrix2D 拷贝) / (a,b,c,d,e,f)
    int g = engine->checkAndSetArgs("null|JZMatrix2D|double,double,double,double,double,double", &r);
    if (g == -1)
        return nullptr;
    if (g == 0)
        return jzNewValue<JZMatrix2D>(ctx, JZMatrix2D());   // 单位阵
    if (g == 1)
        return jzNewValue<JZMatrix2D>(ctx, *static_cast<JZMatrix2D*>(r[0].p));   // 拷贝构造
    JZMatrix2D m;
    m.setMatrix(r[0].d, r[1].d, r[2].d, r[3].d, r[4].d, r[5].d);
    return jzNewValue<JZMatrix2D>(ctx, m);
}