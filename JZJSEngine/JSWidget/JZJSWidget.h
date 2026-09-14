#ifndef JZ_JS_WIDGET_H_
#define JZ_JS_WIDGET_H_

#include <string>

#include "JZColor.h"
#include "quickjs/quickjs.h"

// 颜色字符串解析: '#rgb' / '#rrggbb' / '#rrggbbaa' / 颜色名(black white red green blue yellow
// cyan magenta gray|grey transparent), 无法识别时返回黑色。
// 定义在 JZJSWidgetModule.cpp —— 画笔接口(JZPainter.setPen/setBrush)与本文件的
// JZPen/JZBrush 构造器共用同一套解析(单一来源, 别在别处再抄一份)。
JZColor jzParseColor(const std::string& s);

// ---- JS 侧接口(名字 = 注册处 def 的第一个参数, 即 JS 方法名) ----

// setParentWidget(parentOrNull): 进/出对象树 + 同步所有权 + 登记到模块的控件表。
// 签名是 **JSCFunctionData**(末尾多 magic / func_data 两参): 注册时(defData)把模块指针注进
// func_data[0], 回调里经 JZJSWidgetModule::fromFuncData 取回 —— 进树时由父控件保活接收者的
// JS 包装, 脱离父树时交回 JS 拥有。见 JZJSWidgetModule.h 的控件登记说明。
JSValue js_JZWidget_setParentWidget(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv,
                                    int magic, JSValueConst* func_data);
// parentWidget(): 父控件(借用句柄); 无父返回 null。
JSValue js_JZWidget_parentWidget(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
// window(): 所属顶层窗口(借用句柄); 无窗口返回 null。
JSValue js_JZWidget_window(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
// mapTo(parentOrNull, point) / mapTo(parentOrNull, x, y): parent 为 null 时映射到屏幕坐标。
JSValue js_JZWidget_mapTo(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
// layout(): 当前布局的非拥有视图(同一控件+同一布局返回同一个 JS 对象, 缓存见 JZJSWidget.cpp); 无布局返回真 null。
JSValue js_JZWidget_layout(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
// setLayout(layout): 控件接管布局(/Transfer/)。
// **Qt 语义: 只能设一次** —— 已有布局 / 参数为 null 都直接抛 TypeError(Qt 那边是"警告 + 空操作";
// JS 里报错更有用, 也免得传进来的布局被翻成 C++ 拥有却没人接管)。要换布局先 clearLayout()。
JSValue js_JZWidget_setLayout(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
// clearLayout(): 清空布局(删掉布局对象, 之后 layout() 为 null), 等价 Qt 的 `delete w->layout()`。
// 之前发给 JS 的**视图**(layout() 的返回值)立刻失效; 注意 setLayout 传进去的那份包装不在其列。
JSValue js_JZWidget_clearLayout(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

// ============================================================================
// JS 构造器(C 版本) —— 每个类一个, 覆盖 C++ 侧的**全部构造重载**
// ============================================================================
//
// 为什么要自己写构造器(而不是 ClassBind::create 那套 std::function 工厂):
//   * 直接看 argc/argv 就能按 JS 的实参形态分派到不同 C++ 构造(下面每个类都列了形态),
//     例如 JZLabel: JZLabel(parent) / JZLabel(text, parent) 两个重载;
//   * 报错能用 JS_ThrowTypeError 写准("new JZWindow(w, h, title?) 需要宽高两个数字"),
//     形参表那套只能给"需要 N 个参数";
//   * 一个函数一处, 不用在 .cpp 里散着写 lambda。
//
// **与绑定层的约定(两边必须一致)**:
//   * 控件对象一律是**裸类**实例(不做派生包裹); JS 事件转发由控件的事件过滤器
//     (JZSWidgetEventFilter)负责, 挂在"被 JS 包装过的控件"上, 见 JZJSWidgetModule.h;
//   * 包装: 控件 = JSObjectPointer(守卫指针) + cppOwner=false(JS 拥有, 挂进对象树时翻);
//     布局 = JSRawPointer<JZLayout> + cppOwner=false; 值类型 = 值语义(toJSValue)。
//   * 构造函数**返回对象**即为 new 的结果(quickjs 语义: 构造函数返回对象时用它当结果),
//     所以这里不需要 new_target。
//   * 构造完的对象与旧路径一样会走 onCreated 钩子(JZWidget/JZWindow 靠它拿到 JS 句柄)。
//
// 支持的实参形态(null / undefined / 缺省 都表示"没有父控件"):
//   JZWidget        () / (parent)
//   JZWindow        () / (parent) / (w, h) / (w, h, title) / (w, h, title, parent)
//   JZLabel         () / (parent) / (text) / (text, parent)
//   JZPushButton    () / (parent) / (parent, w, h, text)
//   JZRadioButton   () / (parent) / (parent, text) / (text, parent)
//   JZGroupBox      () / (parent) / (title) / (title, parent)
//   JZSplitter      () / (parent) / (orientation) / (orientation, parent)   0=横 1=纵
//   JZScrollBar     () / (parent) / (orientation) / (orientation, parent)   0=横 1=纵
//   其余控件         () / (parent)   (LineEdit/TextEdit/Checkbox/Switch/Slider/ProgressBar/ComboBox/
//                                     SpinBox/Chart/ImageLabel/ListWidget/TableWidget/
//                                     StackedWidget/TabWidget/ScrollArea)
//   JZLayout        () / (pad) / (pad, spacing)   —— 基类抽象, 与旧行为一致地造 JZVBoxLayout
//   JZVBoxLayout    () / (pad) / (pad, spacing)
//   JZHBoxLayout    同上
//   JZGridLayout    () / (pad) / (pad, gap)
//   JZSelfLayout    () / (pad)
//   JZPoint         () / (x, y)
//   JZSize          () / (w, h)
//   JZRect          () / (x, y) / (x, y, w, h)
//   JZColor         () / (colorEnum) / (r, g, b) / (r, g, b, a)
//   JZImage         () / (filePath) / (bytes: Uint8Array)
//   JZPen           () / (color) / (color, width)
//   JZBrush         () / (color) / (color, opacity)
//   JZFont          () / (size) / (family, size) / (family, size, style)
//   JZMatrix2D      () / (a, b, c, d, e, f) / (other)

JSValue js_JZWidget_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZWindow_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZLabel_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZPushButton_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZLineEdit_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZTextEdit_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZCheckbox_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZRadioButton_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZSwitch_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZSlider_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZProgressBar_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZComboBox_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZSpinBox_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZGroupBox_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZChart_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZImageLabel_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZListWidget_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZTableWidget_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZStackedWidget_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZTabWidget_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZSplitter_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZScrollArea_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZScrollBar_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);

JSValue js_JZLayout_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZVBoxLayout_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZHBoxLayout_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZGridLayout_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZSelfLayout_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);

JSValue js_JZPoint_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZSize_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZRect_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZColor_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZImage_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);

// 画笔样式三件套(值类型, 传给 JZPainter.setPen/setBrush/setFont):
//   JZPen   () / (color) / (color, width)                  color = JZColor 或 '#rrggbb'/颜色名
//   JZBrush () / (color) / (color, opacity)                opacity 0~255(缺省 255 = 不透明)
//   JZFont  () / (size) / (family, size) / (family, size, style)
//          style 为位或: JZFont.Normal 0 / Bold 1 / Italic 2 / Underline 4 / StrikeOut 8
JSValue js_JZPen_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZBrush_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue js_JZFont_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);

// JZMatrix2D (2x3 仿射变换矩阵, 传给 JZPainter.setTransform):
//   () / (a, b, c, d, e, f) / (other: JZMatrix2D)   —— 无参 = 单位阵
//   静态工厂: JZMatrix2D.translation(dx,dy) / scaling(sx,sy) / rotation(deg) / rotationRadians(rad)
JSValue js_JZMatrix2D_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);

// 把上面这些构造器换成各注册类的 JS 构造函数(替换引擎按 create 生成的那个):
//   ctor = JS_NewCFunction2(..., JS_CFUNC_constructor) → JS_SetConstructor(ctor, 类原型)
//   → 再把全局上的类名指到新 ctor(引擎的 classConstructor() 就是按全局名取的, 所以必须换)。
// **必须在 registWidgets / registViews / registLayouts 之后调用**(类要先注册)。
// 旧构造器上的自有属性(JZChart.LineChart/BarChart/PieChart 这类静态常量)会一并搬过来。
void jzRegistWidgetJsConstructors(JSContext* ctx);

#endif // !JZ_JS_WIDGET_H_
