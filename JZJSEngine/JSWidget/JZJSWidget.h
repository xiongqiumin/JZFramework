#ifndef JZ_JS_WIDGET_H_
#define JZ_JS_WIDGET_H_

#include "quickjs/quickjs.h"

// JZWidget 的"指针类"接口 —— 全部做成 **quickjs C 函数**(QJS 的 C 函数签名:
// `JSValue fn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)`)。
//
// ==== 为什么这几个接口必须是 C 函数, 而不是 ClassBind::def 的 lambda ====
// def 注册的方法: lambda 只拿得到"已经转好的形参"(如 JZWidget* parent), 拿不到
//   `this_val` / 原始 JSValue; 而下面这些接口恰恰需要 JS 侧那个**包装对象本身**:
//   * setParentWidget: 挂进对象树后由父对象负责 delete, 必须翻转"接收者自己"的包装
//     所有权(/TransferThis/, JS GC 不能再删) —— 接收者的包装只能从 this_val 取;
//   * parentWidget / window: 没有父/没有窗口时要返回**真正的 null**, 不能造一层
//     "空包装对象"(那是真值, `if (p)` 判不出来, 之后一调用就断言);
//   * layout / setLayout: 布局不是 JZObject(没有守卫指针), 必须显式翻转 /Transfer/,
//     并把"失效开关"存在 JS 对象自己身上 —— 替换布局时让已发出的旧包装立刻失效;
//   * (顺带)C 函数自己校验 this_val/参数, 不经过 ClassBind 的形参类型表 —— 也就不受
//     "形参首参必须是已注册类"那条约束。
//
// ==== 与 JZJS<Native> 镜像类的关系 ====
// 镜像类(JZJSListWidget / JZJSTableWidget 等, 见各自头文件)解决的是"JS 侧创建的**数据项**
// 的所有权/失效", 走的是 def + 包装引用形参那套; 本文件这组接口只服务**控件自身**, 用 C 函数。
// 两边共同的命名约定仍成立: C++ 侧带包装语义的方法以 Js 结尾(本文件按 quickjs 惯例直接叫
// js_JZWidget_xxx), **JS 侧看到的名字一律由注册处决定**(不带后缀)。
//
// 注册方式: 在 JZJSWidgetModule::registWidgets() 里由 ClassBind<JZWidget> 直接挂上去
// (见 JZJSBind.h 的 def(name, JSCFunction*) —— 第一个参数就是 JS 侧方法名)。挂在 JZWidget
// 原型上, JZWindow 等派生类经原型链继承。
// 例外: setParentWidget 还要动模块的控件表(保活 JS 包装), 而 C 函数拿不到模块指针 ——
// 它走**带 func_data 的注册**(defData), 回调多两个参数(magic / func_data), 模块指针在 func_data 里。

// 所有权的运行期状态机(等价 SIP 的 /Transfer/ 与 /TransferBack/, 即包装的 cppOwner 翻转)。
// 经多态基类走: 包装类型随类层次不同(JSRawPointer<具体类> / JSObjectPointer), 不能按一种强转。
// 非包装对象(如传个数字)时静默忽略。
void jzSetCppOwner(JSValueConst value, bool cppOwner);

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
// 为什么要自己写构造器(而不是 ClassBind::createArgs 那套 std::function 工厂):
//   * 直接看 argc/argv 就能按 JS 的实参形态分派到不同 C++ 构造(下面每个类都列了形态),
//     例如 JZLabel: JZLabel(parent) / JZLabel(text, parent) 两个重载;
//   * 报错能用 JS_ThrowTypeError 写准("new JZWindow(w, h, title?) 需要宽高两个数字"),
//     形参表那套只能给"需要 N 个参数";
//   * 一个函数一处, 不用在 .cpp 里散着写 lambda。
//
// **与绑定层的约定(两边必须一致)**:
//   * 控件类里只有 JZWidget / JZWindow 的对象必须是 JSWidgetClass<W>(事件转发靠它:
//     jzBindWidgetInstance<W> 会 static_cast<JSWidgetClass<W>*>); 其余控件是裸类;
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

// 把上面这些构造器换成各注册类的 JS 构造函数(替换引擎按 createArgs 生成的那个):
//   ctor = JS_NewCFunction2(..., JS_CFUNC_constructor) → JS_SetConstructor(ctor, 类原型)
//   → 再把全局上的类名指到新 ctor(引擎的 classConstructor() 就是按全局名取的, 所以必须换)。
// **必须在 registWidgets / registViews / registLayouts 之后调用**(类要先注册)。
// 旧构造器上的自有属性(JZChart.LineChart/BarChart/PieChart 这类静态常量)会一并搬过来。
void jzRegistWidgetJsConstructors(JSContext* ctx);

#endif // !JZ_JS_WIDGET_H_
