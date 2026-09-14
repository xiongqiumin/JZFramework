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
    // 做法: 子控件进树时, 模块在**该控件自己的登记记录**(WidgetInfo::jsvalue)里持一份它的
    //   JS 包装引用(保活); 控件销毁(sigDestroyed)或脱离父树时释放。于是"只要 C++ 对象树还
    //   持有控件, 它的 JS 包装就活着"。
    // 顶层控件(没有父控件的, 一般就是窗口)**不保活**: 它的生死由 JS 决定 —— JS 放手 ->
    //   包装析构 -> 窗口销毁 -> 没有窗口了, runtime 退出(见 gomoku/main.js 的"必须留在模块作用域")。
    //
    // 登记入口(**挂进对象树的每条路都经过它**, 见 bindWidgetToParent):
    //   * takeWidget —— addWidget / insertWidget / addTab / setWidget / StackedWidget.addWidget ...
    //     (见 registWidgets/registLayouts: 形参收 JSValue 的那些"移交所有权"的端口);
    //   * JZWidget.setParentWidget(quickjs C 函数, 经 func_data 拿到本模块, 见 JZJSWidget.cpp);
    //   * registerCreatedWidget —— 构造函数直接带父的(如 new JZWindow(w,h,t,parent))。
    // 不覆盖: ① 顶层控件(见上, 生死由 JS 决定); ② **布局**(JZLayout 不是 JZObject, 进不了这张表,
    //   其包装仍要 JS 侧持引用, uiLoader 生成的 __keep 主要为此)。
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
    // 各控件"额外的扩展状态": 由该控件相关的绑定代码自己派生子类放进来
    // (如列表/表格将来真做 JS 侧 item 接口时的"每项失效开关表"、缓存等)。
    // 状态属于**绑定层** —— 不往控件类上加成员, 也不派生镜像类(见 JZJSWidget.h 顶部"状态放哪")。
    struct WidgetExtState
    {
        virtual ~WidgetExtState() = default;
    };

    // ---- 事件回调槽(JS 侧 `w.onPaint = fn` 注入的回调, 存在模块这边) ----
    // 为什么放 C++ 侧而不是"包装对象上的自有属性": 包装对象是不可靠的 —— getter(area.widget() 等)
    // 会新建包装、包装还可能被 GC; 而"回调挂在哪一份包装上"根本没法从 C 侧判断。
    // 存这里之后: 回调与包装的身份/生命周期**彻底解耦**, 派发时只查槽; 包装只用于两件事 ——
    // 当 this 调回调、以及当 self() 的栈顶。
    enum EventSlot
    {
        Slot_paint = 0,
        Slot_mousePress, Slot_mouseRelease, Slot_mouseMove, Slot_wheel,
        Slot_enter, Slot_leave,
        Slot_keyPress, Slot_keyRelease,
        Slot_focusIn, Slot_focusOut,
        Slot_show, Slot_hide, Slot_resize,
        EventSlot_count
    };
    // 槽位对应的 JS 属性名(= 原来那套 onXxx 名字, 写法不变)
    static const char* eventSlotName(int slot);

    // 一个控件的事件回调槽(模块**持有引用**: setter 注入时 dup, 清除/销毁时 free)
    struct EventCallbacks
    {
        JSValue v[EventSlot_count];

        EventCallbacks()
        {
            for (int i = 0; i < EventSlot_count; i++)
                v[i] = JS_UNDEFINED;
        }
    };

    // 单个控件的 JS 侧状态(键 = object, 一个控件一条)。
    struct WidgetInfo
    {
        JZObject* object = nullptr;      // 底层控件裸指针(不拥有; 顶层窗口由它的 JS 包装拥有)
        JSValue jsvalue = JS_UNDEFINED;  // 模块 dup 的一份 JS 包装引用("保活"); 无父时为空
        // 事件转发 / self() 的目标那份包装(**借用, 模块不释放**): 只在"移交/登记"路径写入
        // (bindWidgetToParent / registerCreatedWidget), 与上面那份保活成对 —— 即"移交给 C++ 时
        // 拿到的那个 JS 对象"。**不要**用 getter(area.widget() / parentWidget() / window() ...)
        // 临时造出来的包装覆盖它: 那些包装没人保活(GC 后就是悬垂)。
        // 注: 回调(onPaint 等)**不在这份包装上** —— 它们在下面的 callbacks 槽里, 所以换目标不会
        // 丢回调; 这份包装只决定回调里 this / self() 是谁(没登记时就现造一份兜底, 见 forwardJs)。
        // 生命周期: 有父时由 jsvalue 保活; 顶层控件(借用)一旦 JS 放手, 包装析构 -> 控件销毁 ->
        // 本节点被摘掉(见 onWidgetDestroyed), 不会留下悬垂句柄。
        JSValue jsRef = JS_UNDEFINED;
        // 该控件的扩展状态(按需创建; 类型由使用方约定, 用 static_cast 取回)
        std::shared_ptr<WidgetExtState> ext;
        // 事件回调槽(onPaint / onMousePress ...): 见上面的 EventSlot
        EventCallbacks callbacks;
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

    // ---- 事件回调槽的读写(注册成属性的 getter/setter 用, 见 WidgetInfo::eventCallbacks) ----
    // setter: 校验是函数(null/undefined = 清空), dup 一份存进槽
    void setWidgetCallback(JZWidget* widget, int slot, JSValueConst cb);
    // getter: 借用值; 没设返回 JS_UNDEFINED
    JSValue widgetCallback(JZWidget* widget, int slot);

    // ---- JS 事件转发的内部实现 ----
    // 该槽设了回调就转发并返回 true(事件被拦下), 否则返回 false(放行走原生实现)
    template <class E>
    bool forwardIf(WidgetInfo& node, int slot, E* event);
    // 以该控件那份包装为 this 调回调; 事件对象包成**借用**包装(栈事件, 派发完即失效)
    template <class E>
    void forwardJs(WidgetInfo& node, int slot, E* event);

    // ---- 控件登记表操作(键 = 控件指针) ----
    // 按指针取节点; 没有返回 nullptr。
    WidgetInfo* findWidgetInfo(JZObject* object);
    // 取节点; 没有就新建(顺带订阅该控件的 sigDestroyed —— 控件销毁时节点自己摘掉)
    WidgetInfo* ensureWidgetInfo(JZObject* object);
    // 释放节点的保活引用(借用型只丢指针)。调用方必须**先把节点从表里摘下来**:
    // 释放可能级联销毁控件, 而那些控件会回调进来摘别的节点。
    void freeWidgetInfo(WidgetInfo& node);
    // 摘掉并释放(控件销毁时调用)
    void removeWidgetInfo(JZObject* object);
    // 把模块从所有登记控件的过滤器表上摘掉(模块先于控件销毁时用; 不碰 JS, 不会级联删控件)
    void detachWidgetFilters();

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

    // 控件登记表: 键 = 控件指针, 值 = 它的 JS 侧状态(保活引用 / 事件转发句柄 / 扩展状态)。
    // 与控件树结构无关 —— 改父、脱离父树都只更新这一条; 控件销毁(sigDestroyed)时摘掉。
    std::map<JZObject*, WidgetInfo> m_widgets;

    // 带 func_data 的 C 函数(setParentWidget)要注入的"模块指针句柄": JS_NewInt64(模块指针)。
    // 指针在 x64 上是 48 位, double(53 位尾数)可精确表示; 且 quickjs 的 float64 **不带引用计数**,
    // GC 的 mark/free 对它都是空操作, 所以可以安全地放进 func_data(JS 侧读不到这份数据)。
    JSValue m_widgetData = JS_UNDEFINED;
};

#endif // !JZ_JS_WIDGET_MODULE_H_
