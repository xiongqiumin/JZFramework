#ifndef JZ_JS_WIDGET_MODULE_H_
#define JZ_JS_WIDGET_MODULE_H_

#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstring>
#include "JZJSEngine.h"
#include "JZColor.h"
#include "JZSignal.h"
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
class JZWidget;
class JZVBoxLayout;
class JZCanvas;
class JZPainter;

// jsbind::ClassBind<C>(defSignalProperty 的形参类型; 定义在 JZJSBind.h, 这里只要前向声明)
namespace jsbind { template <class T> class ClassBind; }

// 把 JZWidget 家族(窗口 + 各控件 + 布局 + JZPainter)注册成 JS 全局类 (事件走类继承 + onXxx 属性写法):
//   const win = new JZWindow(w, h, title);       // 注册类, new 构造(C 构造器按实参形态分派)
//   const pane = new JZWidget();                 // 任意 JZWidget 都能自绘(不限于窗口)
//   pane.onPaint = (e) => { const p = new JZPainter(pane); p.fillRect(...); };  // e.rect() 给出绘制区
//        画笔与 C++ 一致: 在 onPaint 内自建, 只在本次派发期间有效(派发结束即失效)。
//   pane.onMousePress = (e) => { const x = e.localX(), y = e.localY(); };   // 局部逻辑坐标
//   pane.update();                               // 触发重绘
//   win.show();
// 事件转发: 被 JS 包装过的控件会装一个 JZSWidgetEventFilter(见上), 不受控件类限制 ——
//   JZWidget/JZWindow 之外的控件(以及 C++ 侧建好再交给 JS 的控件)同样可以用 onXxx。
// 测宽与绘制都复用框架现有链路, 窗口存在时 runtime 不退出。
// 注: 旧的 JZCanvas / win.createCanvas() 包装已移除(画布改用普通 JZWidget + onPaint + JZPainter);
//     JZJSUiLoader 的 uiClassMap 里 'Canvas' -> 'JZCanvas' 是待清理的残留项。
//     TS 类型声明见 JZJSEngine/js_wrapper/widget/types/(与 registXxx 一一对应)。
class JZJSWidgetModule : public JZObject
{
public:
    JZJSWidgetModule();
    ~JZJSWidgetModule();

    void initEngine(JZJSEngine* engine);
    void deinitEngine();

    JZJSEngine* engine() const { return m_engine; }
    JZPainter* createPainter(JSContext* ctx, int argc, JSValueConst* argv);
    void beginPainter(JZWidget* widget);
    void endPainter();

    // ----------------------------------------------------------------------
    // JS 事件转发(模块自己当过滤器)
    // ----------------------------------------------------------------------
    // 语义(与 C++ 虚函数一一对应): 控件上设了某一项回调(onPaint / onMousePress / onShow ...)
    // 就转发并把事件**拦下**(原生实现不再跑); 没有就放行(false), 走控件自己的实现。
    // 回调存在 C++ 侧(WidgetInfo::callbacks, 由属性 setter 注入), 派发时按槽查 —— 与"回调挂在哪份
    // 包装上"无关; 包装(WidgetInfo::jsRef)只用于当 this 调回调、以及当 self() 的栈顶。
    //
    // 为什么用事件过滤器而不是"派生包裹类 + 成员"(原 JSWidgetClass<W> 那套):
    //   * 包裹类要求把基类指针还原成包裹类型(static_cast<JSWidgetClass<W>*>) —— 而 **C++ 侧 new
    //     出来的控件是裸类对象**(JZGroupBox 的内容容器 `new JZWidget(this)`、C++ 建好再被 JS 拿到
    //     句柄的控件...), 按包裹类写它的成员就是写越界(ASan 实测 heap-buffer-overflow);
    //   * 包裹只对显式实例化的类生效(JZWidget/JZWindow), 其余控件赋 onPaint 静默无效;
    //   * 控件对象上不该多出 JS 语义的成员(engine / JSValue / 绘制作用域) —— 那是绑定层的状态。
    // 过滤器方案: 模块自己就是过滤器, 装在**被 JS 包装过的控件**上(installEventFilter(this),
    // 与 JZSplitter / JZTabWidget 对子控件的做法同一套路); 每个控件的状态(回调槽 / 包装句柄)就放在
    // 它在这棵树里的登记节点上(WidgetInfo) —— 控件与 JS 包装的关联本来就在这棵树上, 不必另立一张表。
    //
    // 事件来源: 框架派发的所有事件都经 JZApplication::sendEvent(JZThread::notify) 进对象,
    // 那里先过过滤器链再进 event()/xxxEvent() —— paint / 鼠标 / 键盘 / 悬停 / 焦点 / 显隐 / 尺寸
    // 都在其中(见 JZWidget::event 的分派表)。
    //
    // 事件对象的包装: 事件是**栈对象**, 只在本次派发期间有效, 故包装成借用指针
    // (JSRawPointer(event, cppOwner=true) —— JS 不负责释放), 派发结束立刻 clear():
    // 之后 JS 若还留着事件对象, 访问它抛异常而不是读悬垂内存。
    bool eventFilter(JZObject* watched, JZEvent* event) override;

    // 给"被 JS 包装的控件"装上事件转发(onCreated 钩子经 jzBindWidgetInstance 调进来):
    // 只是把模块挂成该控件的事件过滤器 + 兜底补登记节点 —— **不动转发目标**(见 WidgetInfo::jsRef:
    // 这个钩子每条造包装的路径都走, 而 getter 造的临时包装没人保活)。
    // 幂等: 重复 installEventFilter 会被忽略。
    void bindJsEventFilter(JZWidget* widget, JSValueConst value);

    // quickjs C 函数的 func_data -> 模块指针(见 jsbind::ClassBind::defData)。
    // 约定: func_data[0] 是"模块指针句柄"(以 JS_NewInt64 存的整数, JS 侧看不到也拿不到)。
    // 取不到时返回 nullptr **并已设 JS 异常**, 调用方直接 `return JS_EXCEPTION`。
    static JZJSWidgetModule* fromFuncData(JSContext* ctx, JSValueConst* func_data);
    
private:    
    enum{ Slot_paint, Slot_mousePress, Slot_mouseRelease, Slot_mouseMove, Slot_wheel, Slot_enter, Slot_leave,
        Slot_keyPress, Slot_keyRelease, Slot_focusIn, Slot_focusOut, Slot_show, Slot_hide, Slot_resize, EventSlot_count
    };
    
    const char* eventSlotName(int slot);
    JSValue slotValue(JSValue value, const char *slot_name);

    JZJSEngine* m_engine = nullptr;
    JZPainter *m_painter = nullptr;
    JZWidget* m_paintWidget = nullptr;

    // ---- JS 事件转发的内部实现 ----
    // 该槽设了回调就转发并返回 true(事件被拦下), 否则返回 false(放行走原生实现)
    template <class E>
    bool forwardIf(JSValue node, const char* name, E* event);
    // 以该控件那份包装为 this 调回调; 事件对象包成**借用**包装(栈事件, 派发完即失效)
    template <class E>
    void forwardJs(JSValue node, const char* name, E* event);

    // 分类注册 (initEngine 中统一调用)
    void registGeometry();   // JZPoint / JZSize / JZRect
    void registColor();      // JZColor
    void registImage();      // JZImage
    void registEvents();     // JZEvent 及其子类
    void registWidgets();    // JZWidget / JZWindow 及基础控件(Label/Button/Slider/.../Chart)
    void registDialogs();    // JZDialog + 全局 JZMessageBox / JZFileDialog(模态对话框)
    void registSettings();   // JZSettings(INI 配置, 对应 QSettings)    
    void registApp();        // 全局 jzApp(JZApplication 的应用级入口: 路径 / quit / exit)
    void registViews();      // 容器/视图类控件(List/Table/Tab/Splitter/ScrollArea/ScrollBar/...)
    void registLayouts();    // JZLayout 及其四个具体布局
    void registPainter();    // JZPainter
    void registPens();       // JZPen / JZBrush / JZFont (画笔样式值类型)
    void registMatrix();     // JZMatrix2D (2D 仿射变换矩阵值类型)
    void registUiLoader();   // JZJSUiLoader + 全局 uiLoader / jzWidget 命名空间

    // ---------- JS 信号回调 ----------
    // 语义: 信号也是**属性**(`btn.onClicked = fn`), 与事件槽同一条规则 —— 赋值设、覆盖旧的、
    // null 清空、可读回。实现: 首次给某个信号属性赋值时才 connect 一次, lambda 每次去查槽
    // (空槽直接跳过, 所以"清空"不需要断开连接)。

    // 调用 JS 回调一次。argv 里的值由本函数负责释放(与 JS_Call 的"借用"语义相配)。
    // fn 是**借用**值: 调用期间自己 dup 一份 —— 于是回调里把同一个信号属性重设/清空(槽里的
    // 旧值被 free)也不会让正在执行的函数对象悬垂。
    // owner = 信号所属对象(见 JZSignal::object()): 调用期间把它压成"当前 this", 于是回调里
    // self() 拿到的就是"发信号的那个控件/对象" —— 箭头函数写的 onClicked 也能认出自己是谁。
    JSValue callJsCallback(JSValueConst fn, JZObject* owner, int argc, JSValueConst* argv);

    // 把 C++ 信号接到槽上(按信号形状重载; JS 侧属性名由注册处给出)。幂等: 同一个(对象, 信号)
    // 只 connect 一次 —— 之后 setter 只换槽里的值, 空槽在触发时直接跳过, 所以"清空"不必断开连接。
    void ensureConnected(JZObject* owner, JZSigVoid& sig, const char* name);
    void ensureConnected(JZObject* owner, JZSigBool& sig, const char* name);
    void ensureConnected(JZObject* owner, JZSigInt& sig, const char* name);
    void ensureConnected(JZObject* owner, JZSigString& sig, const char* name);
    void ensureConnected(JZObject* owner, JZSignal<int, int>& sig, const char* name);

    // 注册一个"信号属性"的样板(按信号形状分派到上面的 ensureConnected):
    // getter 读槽(slotToJs) / setter 写槽(setSlot) + 首次赋值时 connect。各控件注册处因此只需一行:
    //   defSignalProperty(bind_spin, "onValueChanged", &JZSpinBox::sigValueChanged);
    // 两个模板形参: C = 注册的类(与 ClassBind 一致), S = 信号**声明的**类 —— 二者常常不同
    // (如 sigTextChanged 声明在 JZAbstractTextEdit 上而注册在 JZLineEdit 上), S 必须自己推导。
    template <class C, class S> void defSignalProperty(jsbind::ClassBind<C>& bind, const char* name, JZSigVoid S::* sig);
    template <class C, class S> void defSignalProperty(jsbind::ClassBind<C>& bind, const char* name, JZSigBool S::* sig);
    template <class C, class S> void defSignalProperty(jsbind::ClassBind<C>& bind, const char* name, JZSigInt S::* sig);
    template <class C, class S> void defSignalProperty(jsbind::ClassBind<C>& bind, const char* name, JZSigString S::* sig);
    template <class C, class S> void defSignalProperty(jsbind::ClassBind<C>& bind, const char* name, JZSignal<int, int> S::* sig);

    // 注册一个"信号属性"的样板(按信号形状分派到 5 个重载, 与上面的 ensureConnected 对应):
    //   getter 读槽(slotToJs) / setter 写槽(setSlot) + 首次赋值时 connect。
    // 各控件注册处因此只需一行, 例如:
    //   defSignalProperty(bind_btn, "onClicked", &JZPushButton::sigClicked);
    // 两个模板形参: C = 注册的类(与 ClassBind 一致), S = 信号**声明的**类 —— 二者常常不同
    // (如 sigTextChanged 声明在 JZAbstractTextEdit 上, 而注册在 JZLineEdit 上; 成员指针的宿主
    //  类型就是基类), S 必须自己推导, 否则形参匹配不上。    

    // 给类的**构造器**挂静态函数(JS 侧 `JZImage.create(...)`)。
    // def() 注册的是原型方法, 而 JS 里 `JZImage.create` 走的是函数对象自身的原型链
    // (Function.prototype), 取不到原型上的方法 —— 静态入口必须挂在构造器上
    // (与 JZChart 的类型常量同一处 JS_SetPropertyStr)。
    void defStaticFunction(const CClassDefine& define, const char* name, JSCFunction* fn, int length);

    // 带 func_data 的 C 函数(setParentWidget)要注入的"模块指针句柄": JS_NewInt64(模块指针)。
    // 指针在 x64 上是 48 位, double(53 位尾数)可精确表示; 且 quickjs 的 float64 **不带引用计数**,
    // GC 的 mark/free 对它都是空操作, 所以可以安全地放进 func_data(JS 侧读不到这份数据)。
    JSValue m_widgetData = JS_UNDEFINED;
};

#endif // !JZ_JS_WIDGET_MODULE_H_
