#ifndef JZ_JS_WIDGET_H_
#define JZ_JS_WIDGET_H_

#include <string>

#include "JZColor.h"
#include "quickjs/quickjs.h"
#include "JZJSType.h"

// 颜色字符串解析: '#rgb' / '#rrggbb' / '#rrggbbaa' / 颜色名(black white red green blue yellow
// cyan magenta gray|grey transparent), 无法识别时返回黑色。
// 定义在 JZJSWidgetModule.cpp —— 画笔接口(JZPainter.setPen/setBrush)与本文件的
// JZPen/JZBrush 构造器共用同一套解析(单一来源, 别在别处再抄一份)。
JZColor jzParseColor(const std::string& s);

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
//     布局 = JSRawPointer<JZLayout> + cppOwner=false; 值类型 = 值语义(tovoid *)。
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

void *js_JZWidget_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZWindow_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZLabel_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZPushButton_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZLineEdit_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZTextEdit_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZCheckbox_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZRadioButton_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZSwitch_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZSlider_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZProgressBar_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZComboBox_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZSpinBox_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZGroupBox_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZChart_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZImageLabel_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZListWidget_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZTableWidget_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZStackedWidget_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZTabWidget_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZSplitter_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZScrollArea_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZScrollBar_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);

void *js_JZLayout_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZVBoxLayout_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZHBoxLayout_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZGridLayout_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZSelfLayout_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);

void *js_JZPoint_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZSize_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZRect_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZColor_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZImage_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);

// 画笔样式三件套(值类型, 传给 JZPainter.setPen/setBrush/setFont):
//   JZPen   () / (color) / (color, width)                  color = JZColor 或 '#rrggbb'/颜色名
//   JZBrush () / (color) / (color, opacity)                opacity 0~255(缺省 255 = 不透明)
//   JZFont  () / (size) / (family, size) / (family, size, style)
//          style 为位或: JZFont.Normal 0 / Bold 1 / Italic 2 / Underline 4 / StrikeOut 8
void *js_JZPen_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZBrush_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);
void *js_JZFont_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);

// JZMatrix2D (2x3 仿射变换矩阵, 传给 JZPainter.setTransform):
//   () / (a, b, c, d, e, f) / (other: JZMatrix2D)   —— 无参 = 单位阵
//   静态工厂: JZMatrix2D.translation(dx,dy) / scaling(sx,sy) / rotation(deg) / rotationRadians(rad)
void *js_JZMatrix2D_ctor(JZJSEngine *engine, int argc, JSValueConst *argv);


#endif // !JZ_JS_WIDGET_H_
