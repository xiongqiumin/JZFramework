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

// ============================================================================
// 宿主句柄: C++ 对象上挂的"反向引用"(JS 对象), 存在对象的动态属性 "js_value" 里
// ============================================================================
// 为什么放 C++ 对象上: 事件/信号派发时要**从 JS 对象现读属性**(w.onPaint = fn 直接就是普通 JS
// 属性, 不再往 C++ 侧抄一份回调), getter 也要能返回"同一个 JS 对象"(`===` 成立)。
//
// 引用是**借用**(不 dup / 不 free): 所有权一直在 JS 侧 —— JS 对象被回收时, 它 opaque 里的守卫
// 会 delete 底层 C++ 对象(见 JZObjectPointer 的处理), 于是"JS 对象死"与"C++ 对象死"是同一件事,
// 这份借用值不可能被读到失效的时候。反过来若 C++ 持强引用, JS 对象就永远回收不掉 -> 泄漏。
class JZJSHandle
{
public:
    JZJSHandle() {}
    JZJSHandle(JZJSEngine* engine, JSValueConst value) : m_engine(engine), m_value(value) {}

    bool isNull() const { return !m_engine || JS_IsUndefined(m_value) || JS_IsNull(m_value); }
    JZJSEngine* engine() const { return m_engine; }
    JSValue value() const { return m_value; }      // 借用: 调用方不要 free

private:
    JZJSEngine* m_engine = nullptr;
    JSValue m_value = JS_UNDEFINED;
};

JZ_DECLARE_METATYPE(JZJSHandle)

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

    // 把 JS 对象登记到控件上(对象动态属性 "js_value") + 给控件装事件过滤器。幂等。
    // 触发点: ① 控件被 JS 构造出来时(引擎 create 之后的回调, 见 onJsObjectCreated);
    //         ② 绑定层自己造 JS 视图时(如 getter 返回已有控件)。
    void bindJsEventFilter(JZWidget* widget, JSValueConst value);

    // 引擎造出 JS 对象之后的回调(绑定层用它把 js_value 记到对象上; 见 JZJSEngine::setObjectCreatedCallback)。
    // 承载"JS 侧对象先存在"这一新思路: 对象一诞生就有宿主句柄, 事件/信号/所有权翻转都靠它。
    void onJsObjectCreated(int type, JSValue object, void* ptr);

    // 宿主句柄读写(借用值; 没有返回 JS_UNDEFINED)
    JSValue jsValueOf(JZObject* object);
    void attachJsValue(JZObject* object, JSValueConst value);

    // 布局 addWidget 的实参: 校验是控件并取裸指针(拿不到抛 JS 异常 + 返回 nullptr)。
    JZWidget* takeWidgetFromJs(JSValue value);

    // ========================================================================
    // 透明控件类: 控件族对 JZJSEngine **完全透明** —— 不 registClass、不进 m_typeMap, 引擎的
    // checkType / isInstanceOf / 形参表一律不认控件对象; 类型契约交给 TS 侧(d.ts), 运行期只做兜底校验。
    // C++ 只提供两样东西:
    //   ① 基构造器 `__JZWidgetObject` —— 造出"带 op 槽"的实例(TS 侧的 class 用 super() 调它,
    //      于是 this 天然带 opaque, 见下面 setOp 的说明);
    //   ② 一批**原生 quickjs 函数** —— `init_c_xxx(this, ...)`(造底层对象 + setOp)与各原型方法。
    // JS 侧形状(TS):
    //   class JZWidget extends __JZWidgetObject {
    //     constructor(nocreate?: boolean) { super(); if (!nocreate) init_c_widget(this); }
    //   }
    //   class JZLabel extends JZWidget {
    //     constructor(text?: string, parent?: JZWidget | null) { super(true); init_c_label(this, text, parent); }
    //   }
    // C 侧形状:
    //   void init_c_label(JSValueConst jsThis, ...) { JZLabel* w = new JZLabel(...); setOp(jsThis, w); }
    //   JSValue c_label_setText(JSContext* ctx, JSValueConst jsThis, int argc, JSValueConst* argv)
    //   { JZLabel* w = widgetFromThis<JZLabel>(ctx, jsThis, "setText"); if (!w) return JS_EXCEPTION; ... }
    // ========================================================================
    JSClassID widgetOpClassId() const { return m_opClassId; }

    // op 槽 = JS 对象 opaque 里的**守卫**(JZObjectPointer*), 由 init_c_xxx 写入; 空槽 = 还没建底层对象。
    // 为什么不是裸指针: 底层对象可能先被 C++ 删掉(父控件 delete 子控件), 守卫能认出"已经没了" ——
    // 调用时抛异常而不是 UAF, 终结时也只会删还活着的那个(防重复删除)。
    // 注意: opaque 只有**自定义 class** 的实例才有(quickjs 的 JS_SetOpaque 对普通 JS 对象返回 -1),
    //       所以实例必须由基构造器造(见上面的形状), 不能是 `class X {}` 直接造出来的普通对象。
    bool setOp(JSValueConst jsThis, void* object);
    // this -> 底层对象; 槽空 / 守卫失效 / 不是控件对象时**已抛 JS 异常**并返回 nullptr(调用方 return JS_EXCEPTION)
    void* opFromThis(JSContext* ctx, JSValueConst jsThis, const char* what);
    // 从形参取控件:**不设 JS 异常**, 只回 bool(null/非控件/已销毁 -> false)。
    // 给"抛 C++ 异常"的老路径用(那些路径的 catch 会自己转 JS 异常, 这里再设 pending 异常会打架)。
    bool widgetValue(JSValueConst v, JZWidget*& out);

    // JZPainter.end() 的托管侧收尾(见 js_JZPainterEnd): 若 view 正是本次绘制派发交给 JS 的那份
    // 屏幕画笔, 摘掉托管(m_painter 交给调用方 delete)并作废视图, 返回它; 不是则返回 nullptr。
    JZPainter* takePaintScopedPainter(JSValueConst view);
    template <class W>
    W* widgetFromThis(JSContext* ctx, JSValueConst jsThis, const char* what)
    {
        return dynamic_cast<W*>(static_cast<JZObject*>(opFromThis(ctx, jsThis, what)));
    }

    // ⛔ 标量/字符串形参**不做类型校验**: 类型契约在 TS 侧(d.ts), C 侧只管取值 ——
    //    直接用 fromJSValue<int/double/bool/std::string> 即可(它们就是 JS_ToInt32/ToFloat64/ToBool/
    //    ToCString, 不会断言, 类型不对是 JS 侧的问题)。
    // 唯一保留校验的是**控件形参**: 极端情况(传 null / 传别的对象)下不能让 C++ 拿到空指针/野指针。
    bool argWidget(JSContext* ctx, JSValueConst v, JZWidget*& out, const char* what, bool allowNull);

    // 往原型上挂一个原生方法(就是 JS_NewCFunction, 不经引擎的形参表/magic 分发)
    void defNative(JSValue proto, const char* name, JSCFunction* fn, int length);
    // 建 op 槽所需的类与基构造器(initEngine 里调)
    void initWidgetOpClass();
    // 加载 TS 类层(js_wrapper/widget/dist/classes.js —— 定义全局控件类), 之后 C++ 把方法挂到这些类
    // 的原型上。返回 false = 类层没装上(缺 dist / 没设 libraryDir), 调用方据此报错。
    bool loadWidgetClasses();
    // 方法挂完后加载包入口(js_wrapper/widget/dist/index.js): 装 JS 便利层(对象树登记/保活, tree.ts)。
    // 顺序不能反 —— 便利层装载时会把原生方法缓存下来。
    bool loadWidgetShell();

    // ========================================================================
    // 把一个"类描述"(CClassDefine)挂到 TS 类层的对应类上 —— **不注册进引擎**。
    // 与 registClass 的分工:
    //   registClass(define)          : 建类 + 原型 + 形参表 + magic 分发, 全进引擎 (旧路径);
    //   mountTransparentClass(define): 只把 define 里的方法/裸函数挂到**已存在的 JS 类原型**上,
    //                                  并把 define.create 注册成全局 init_c_<蛇形>(this, ...)。
    // 方法的实参转换仍复用 CFunction(与旧路径同一份行为), 只是没有引擎的表/magic —— "绕过引擎"。
    void mountTransparentClass(const CClassDefine& define);

    // 挂载后的调用入口(trampoline 用; index 是模块内的方法表下标)
    JSValue callTransparentMethod(int index, JSValueConst thisVal, int argc, JSValueConst* argv);
    JSValue callTransparentInit(int index, JSValueConst thisVal, int argc, JSValueConst* argv);

    // 透明类的方法表(保活 CFunction: 它的 name 是 quickjs 用的裸指针宿主)
    std::vector<std::shared_ptr<CFunction>> m_methods;
    std::vector<CRawFunction> m_rawMethods;
    struct JzInitEntry
    {
        std::string className;                            // JS 类名(信号/事件按它挂)
        decltype(CClassDefine::create) create;             // 复用既有 js_JZxxx_ctor(类型与注册处一致)
    };
    std::vector<JzInitEntry> m_inits;

    // quickjs C 函数的 func_data -> 模块指针(见 jsbind::ClassBind::defData)。
    // 约定: func_data[0] 是"模块指针句柄"(以 JS_NewInt64 存的整数, JS 侧看不到也拿不到)。
    // 取不到时返回 nullptr **并已设 JS 异常**, 调用方直接 `return JS_EXCEPTION`。
    static JZJSWidgetModule* fromFuncData(JSContext* ctx, JSValueConst* func_data);
    
private:    
    enum{ Slot_paint, Slot_mousePress, Slot_mouseRelease, Slot_mouseMove, Slot_wheel, Slot_enter, Slot_leave,
        Slot_keyPress, Slot_keyRelease, Slot_focusIn, Slot_focusOut, Slot_show, Slot_hide, Slot_resize, EventSlot_count
    };
    
    const char* eventSlotName(int slot);

    // 从一个 JS 对象上按属性名取回调(借用值; 不是函数 -> JS_UNDEFINED)。
    // 事件(onPaint...)与信号(onClicked...)共用: JS 侧写法就是普通属性赋值, C++ 侧不存回调。
    JSValue slotValue(JSValue value, const char* slot_name);
    // 该控件"这一项"有没有回调(有就转发并把事件拦下)。JS 对象没了/属性不是函数 -> false(走原生实现)。
    bool hasCallback(JZObject* object, const char* name);

    JZJSEngine* m_engine = nullptr;

    // op 槽(透明控件类)用的自定义 class id: 控件族所有实例共用它 —— 类的身份由 TS 侧的原型链表达,
    // C++ 侧只认"这是不是我们的实例"(opaque 里有没有守卫)。见 initWidgetOpClass。
    JSClassID m_opClassId = 0;
    JZPainter *m_painter = nullptr;
    JZWidget* m_paintWidget = nullptr;
    // 本此派发里交给 JS 的那份 JZPainter 视图(见 onJsObjectCreated / endPainter): 派发结束时
    // 底层对象由模块 delete, 这份视图要 disarmOpaque 作废, 否则 JS 侧会读到悬垂指针、或在 GC 时重复 delete。
    JSValue m_paintView = JS_UNDEFINED;

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

public:
    // ---------- JS 信号回调 ----------
    // (这一小段是 public: 信号登记表里的 lambda 是**自由函数**, 要能调 ensureConnected)
    // 语义: 信号也是**属性**(`btn.onClicked = fn`), 与事件同一条规则 —— 赋值设、覆盖旧的、null 清空。
    // 实现: 对象诞生时把该类(含祖先类)登记过的信号 connect **一次**, lambda 每次去 JS 对象上
    // 现读属性并调用(读到的不是函数就跳过, 所以"清空"不需要断开连接)。

    // 调用 JS 回调一次。argv 里的值由本函数负责释放(与 JS_Call 的"借用"语义相配)。
    // fn 是**借用**值: 调用期间自己 dup 一份 —— 于是回调里把同一个信号属性重设/清空也不会
    // 让正在执行的函数对象悬垂。
    // owner = 信号所属对象(见 JZSignal::object()): 调用期间把它压成"当前 this", 于是回调里
    // self() 拿到的就是"发信号的那个控件/对象" —— 箭头函数写的 onClicked 也能认出自己是谁。
    JSValue callJsCallback(JSValueConst fn, JZObject* owner, int argc, JSValueConst* argv);

    // 把 C++ 信号接到"读 JS 属性再调"的 lambda 上(按信号形状重载)。由 connectSignals 在对象
    // 诞生时调一次, 所以这里不需要维护"已连过"的标记。
    void ensureConnected(JZObject* owner, JZSigVoid& sig, const char* name);
    void ensureConnected(JZObject* owner, JZSigBool& sig, const char* name);
    void ensureConnected(JZObject* owner, JZSigInt& sig, const char* name);
    void ensureConnected(JZObject* owner, JZSigString& sig, const char* name);
    void ensureConnected(JZObject* owner, JZSignal<int, int>& sig, const char* name);

    // 注册一个信号(各控件注册处一行):
    //   defSignalProperty(bind_btn, "onClicked", &JZPushButton::sigClicked);
    // 两个模板形参: C = 注册的类(与 ClassBind 一致), S = 信号**声明的**类 —— 二者常常不同
    // (如 sigTextChanged 声明在 JZAbstractTextEdit 上而注册在 JZLineEdit 上)。
    template <class C, class S, class Sig>
    void defSignalProperty(jsbind::ClassBind<C>& bind, const char* name, Sig S::* sig);

    // 把该对象"所属类及其祖先类"登记过的信号全部 connect 一次(创建对象时调):
    // 按 **JS 类名** + JS_IsInstanceOf 认类(控件族对引擎透明, 不能用引擎的类型表)。
    void connectSignals(JSValue jsThis, JZObject* object);
    // 往登记表里放一条信号(键 = JS 类名)
    template <class S, class Sig>
    void registSignal(const char* jsClassName, const char* name, Sig S::* sig);
    // 登记表: JS 类名 -> 该类的信号连接器
    std::map<std::string, std::vector<std::function<void(JZObject*, JZJSWidgetModule*)>>> m_signalBinders;

private:
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
