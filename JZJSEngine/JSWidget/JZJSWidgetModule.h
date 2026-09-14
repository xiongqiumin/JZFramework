#ifndef JZ_JS_WIDGET_MODULE_H_
#define JZ_JS_WIDGET_MODULE_H_

#include <map>
#include <memory>
#include <string>
#include <vector>
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
#include "JZJSPaintScope.h"

class JZJSEngine;
class JZWindow;
class JZWidget;
class JZVBoxLayout;
class JZCanvas;
class JZPainter;

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

    // ======================================================================
    // 控件 JS 包装的"保活"登记 (C++ 对象树 <-> JS 包装 的循环引用)
    // ======================================================================
    // 问题: 控件挂进对象树后由**父对象**负责 delete(包装被翻成 cppOwner=true, /Transfer/),
    //   但 JS 侧一旦放手(局部变量出作用域 / 引用被覆盖), 包装就会被 GC 收走; 而模块还持有包装
    //   句柄(WidgetInfo::jsRef)用于事件转发(onPaint/onMousePress/...) —— 之后每次都往一个已死的
    //   JS 对象上派发(实测表现为事件静默失效 / 收尾时堆校验报错)。
    // 做法: 子控件进树时, 由**父控件的登记节点**持有一份它的 JS 包装引用(保活);
    //   控件销毁(sigDestroyed)时释放。于是"只要 C++ 对象树还持有控件, 它的 JS 包装就活着"。
    // 顶层控件(没有父控件的, 一般就是窗口)**不保活**: 它的生死由 JS 决定 —— JS 放手 ->
    //   包装析构 -> 窗口销毁 -> 没有窗口了, runtime 退出(见 gomoku/main.js 的"必须留在模块作用域")。
    //
    // 登记入口: takeWidget(布局/窗口加控件时, 见 registWidgets/registLayouts) 与
    //   JZWidget.setParentWidget(quickjs C 函数, 经 func_data 拿到本模块, 见 JZJSWidget.cpp)。
    // ----------------------------------------------------------------------

    // 子控件进对象树: 校验 + 翻包装所有权(/Transfer/) + 登记(保活)。失败抛 std::runtime_error(转 JS 异常)。
    JZWidget* takeWidget(JZWidget* parent, JSValueConst value, const char* what);
    // 把 child 登记到 parent 节点下并保活其 JS 包装(childValue); 已在别处登记时先摘下来(改父)。
    void bindWidgetToParent(JZWidget* parent, JZWidget* child, JSValueConst childValue);
    // 控件脱离父树(setParentWidget(null)): 撤销登记与保活(包装交回 JS 拥有)。
    // 注意: 撤销保活可能让包装立刻析构, 从而 delete 该控件 —— 调用方必须把本调用放在最后。
    void unbindWidget(JZWidget* child);

    // 控件构造完成后登记(js_JZWidget_ctor 系列用, 是 addWidget/setParentWidget 之外的第三条入口):
    //   有父控件(构造时就带 parent) → 挂到父节点下并保活它的 JS 包装;
    //   没有父控件 → 建一个顶层节点(jsvalue 恒空 = 不保活, 生死由 JS 决定)。
    void registerCreatedWidget(JZWidget* widget, JSValueConst value);

    // ----------------------------------------------------------------------
    // JS 事件转发(模块自己当过滤器)
    // ----------------------------------------------------------------------
    // 语义(与 C++ 虚函数一一对应): 控件上实现了同名 JS 方法(onPaint / onMousePress / onShow ...)
    // 就转发并把事件**拦下**(原生实现不再跑); 没有就放行(false), 走控件自己的实现。
    //
    // 为什么用事件过滤器而不是"派生包裹类 + 成员"(原 JSWidgetClass<W> 那套):
    //   * 包裹类要求把基类指针还原成包裹类型(static_cast<JSWidgetClass<W>*>) —— 而 **C++ 侧 new
    //     出来的控件是裸类对象**(JZGroupBox 的内容容器 `new JZWidget(this)`、C++ 建好再被 JS 拿到
    //     句柄的控件...), 按包裹类写它的成员就是写越界(ASan 实测 heap-buffer-overflow);
    //   * 包裹只对显式实例化的类生效(JZWidget/JZWindow), 其余控件赋 onPaint 静默无效;
    //   * 控件对象上不该多出 JS 语义的成员(engine / JSValue / 绘制作用域) —— 那是绑定层的状态。
    // 过滤器方案: 模块自己就是过滤器, 装在**被 JS 包装过的控件**上(installEventFilter(this),
    // 与 JZSplitter / JZTabWidget 对子控件的做法同一套路); 每个控件的转发状态就放在它在这棵树里的
    // 登记节点上(WidgetInfo::jsRef) —— 控件与 JS 包装的关联本来就在这棵树上, 不必另立一张表。
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
    // 登记节点上记下最新那份包装(WidgetInfo::jsRef), 并把模块挂成该控件的事件过滤器。
    // 幂等: 同一控件可能被多次包装(parentWidget() / contentWidget() 这类返回值的转换),
    // 每次都只更新 jsRef(重复 installEventFilter 本身也会被忽略)。
    void bindJsEventFilter(JZWidget* widget, JSValueConst value);

    // quickjs C 函数的 func_data -> 模块指针(见 jsbind::ClassBind::defData)。
    // 约定: func_data[0] 是"模块指针句柄"(以 JS_NewInt64 存的整数, JS 侧看不到也拿不到)。
    // 取不到时返回 nullptr **并已设 JS 异常**, 调用方直接 `return JS_EXCEPTION`。
    static JZJSWidgetModule* fromFuncData(JSContext* ctx, JSValueConst* func_data);

private:
    // 控件登记节点: 与控件树同构(顶层窗口是根, 挂在它下面的子控件各自保活)。
    struct WidgetInfo
    {
        JZObject* object = nullptr;      // 底层控件裸指针(不拥有; 顶层窗口由它的 JS 包装拥有)
        JSValue jsvalue = JS_UNDEFINED;  // 模块 dup 的一份 JS 包装引用("保活"); 顶层节点恒为空
        // 事件转发用的那份包装(**借用, 模块不释放**): 谁被 JS 包装过就记最新那一份。
        // 生命周期靠上面两条: 子控件的包装由 jsvalue 保活; 顶层控件一旦 JS 放手, 包装析构 ->
        // 控件销毁 -> 本节点被摘掉(见 onWidgetDestroyed), 不会留下悬垂句柄。
        JSValue jsRef = JS_UNDEFINED;
        std::vector<WidgetInfo> children;
    };

    JZJSEngine* m_engine = nullptr;
    int m_typeWindow = -1;
    int m_typeCanvas = -1;

    // JS 侧画笔的创建分发与生命周期(registPainter 的请求全部转发到这里, 这个作用域也界定
    // JS 侧 onPaint 里那些画笔的生命周期)
    JZJSPaintScope m_paintScope;

    // 转接给 JS 的信号回调(JS 函数值, 全部 dup 持有一份)。
    // 信号槽本身不持有 JSValue 的生命周期, 而回调可能在很久以后才触发 —— 不 dup 会被 GC 收走;
    // 又因为只在引擎存活期间有效, 统一登记后由 deinitEngine() 一次性释放。
    std::vector<JSValue> m_jsCallbacks;

    void onWidgetDestroyed(JZObject *object);

    // ---- JS 事件转发的内部实现 ----
    // JS 侧是否实现了名为 name 的方法(onPaint / onMousePress ...)
    bool hasJsMethod(JSValue obj, const char* name);
    // 实现了就转发并返回 true(事件被拦下), 否则返回 false(放行走原生实现)
    template <class E>
    bool forwardIf(JSValue obj, const char* name, E* event);
    // 以 obj 为 this 调用 JS 方法, 参数是**借用**包装的事件对象(栈事件, 派发完即失效)
    template <class E>
    void forwardJs(JSValue obj, const char* name, E* event);

    // ---- WidgetInfo 登记树操作 ----
    // 在 list(含子树)里按底层指针找节点; 找不到返回 nullptr。
    WidgetInfo* findWidgetInfoIn(std::vector<WidgetInfo>& list, JZObject* object);
    // 从 list(含子树)里把 object 的节点**摘下来**(连同子树, 不释放任何 jsvalue), move 到 out。
    bool takeWidgetInfoIn(std::vector<WidgetInfo>& list, JZObject* object, WidgetInfo& out);
    // 新建节点并订阅该控件的 sigDestroyed: 节点一建就订阅, 之后无论它挂在谁下面(顶层/子控件),
    // 控件销毁时都会被摘掉并放掉保活引用。重复订阅无害(onWidgetDestroyed 按指针查, 幂等)。
    WidgetInfo* pushWidgetInfo(std::vector<WidgetInfo>& list, JZObject* object);
    WidgetInfo* findWidgetInfo(JZObject* object);
    // 找 object 的节点; 没有就新建(有父控件则挂到父节点下, 否则作为顶层节点)。
    WidgetInfo* ensureWidgetInfo(JZObject* object);
    // 摘下 object 的节点(不释放)
    bool takeWidgetInfo(JZObject* object, WidgetInfo& out);
    // 释放节点与子树里所有保活引用(不 delete 控件本身)
    void freeWidgetInfo(WidgetInfo& node);
    // 摘除并释放(控件销毁时调用)
    void removeWidgetInfo(JZObject* object);
    // 把模块从所有登记控件的过滤器表上摘掉(模块先于控件销毁时用; 不碰 JS, 不会级联删控件)
    void detachWidgetFilters(std::vector<WidgetInfo>& list);

    // 分类注册 (initEngine 中统一调用)
    void registGeometry();   // JZPoint / JZSize / JZRect
    void registColor();      // JZColor
    void registImage();      // JZImage
    void registEvents();     // JZEvent 及其子类
    void registWidgets();    // JZWidget / JZWindow 及基础控件(Label/Button/Slider/.../Chart)
    void registViews();      // 容器/视图类控件(List/Table/Tab/Splitter/ScrollArea/ScrollBar/...)
    void registLayouts();    // JZLayout 及其四个具体布局
    void registPainter();    // JZPainter
    void registPens();       // JZPen / JZBrush / JZFont (画笔样式值类型)
    void registMatrix();     // JZMatrix2D (2D 仿射变换矩阵值类型)
    void registUiLoader();   // JZJSUiLoader + 全局 uiLoader / jzWidget 命名空间

    // ---------- JS 信号回调 ----------

    // 登记一份 JS 回调(dup), 返回值供 connect 的 lambda 捕获; deinitEngine 统一释放。
    JSValue holdJsCallback(JSValue cb);
    // 调用 JS 回调一次。argv 里的值由本函数负责释放(与 JS_Call 的"借用"语义相配)。
    // owner = 信号所属对象(见 JZSignal::object()): 调用期间把它压成"当前 this", 于是回调里
    // self() 拿到的就是"发信号的那个控件/对象" —— 箭头函数写的 onClicked 也能认出自己是谁。
    JSValue callJsCallback(JSValue fn, JZObject* owner, int argc, JSValueConst* argv);

    // 把 C++ 信号接到 JS 回调上(按信号形状重载; JS 侧方法名由各 def() 决定)。
    void bindSignal(JZSigVoid& sig, JSValue cb);
    void bindSignal(JZSigBool& sig, JSValue cb);
    void bindSignal(JZSigInt& sig, JSValue cb);
    void bindSignal(JZSigString& sig, JSValue cb);
    void bindSignal(JZSignal<int, int>& sig, JSValue cb);

    // 给类的**构造器**挂静态函数(JS 侧 `JZImage.create(...)`)。
    // def() 注册的是原型方法, 而 JS 里 `JZImage.create` 走的是函数对象自身的原型链
    // (Function.prototype), 取不到原型上的方法 —— 静态入口必须挂在构造器上
    // (与 JZChart 的类型常量同一处 JS_SetPropertyStr)。
    void defStaticFunction(const CClassDefine& define, const char* name, JSCFunction* fn, int length);

    // 顶层节点(没有父控件的控件, 一般就是窗口); 子控件挂在各自父节点的 children 里
    std::vector<WidgetInfo> m_widgetList;

    // 带 func_data 的 C 函数(setParentWidget)要注入的"模块指针句柄": JS_NewInt64(模块指针)。
    // 指针在 x64 上是 48 位, double(53 位尾数)可精确表示; 且 quickjs 的 float64 **不带引用计数**,
    // GC 的 mark/free 对它都是空操作, 所以可以安全地放进 func_data(JS 侧读不到这份数据)。
    JSValue m_widgetData = JS_UNDEFINED;
};

#endif // !JZ_JS_WIDGET_MODULE_H_
