#ifndef JZ_JS_PAINT_SCOPE_H_
#define JZ_JS_PAINT_SCOPE_H_

#include <vector>

#include "quickjs/quickjs.h"
#include "JZJSType.h"
#include "JZPainter.h"

class JZJSEngine;

// JS 侧 JZPainter 的"创建分发 + 生命周期"管理器, 由 JZJSWidgetModule 持有一份(随模块一起存亡,
// 一个引擎一份)。JZPainter 的注册(registPainter)把创建请求全部转发到这里。
//
// ---- 两种画笔, 两套生命周期 ----
// 1) 屏幕画笔 `new JZPainter(widget)`: 等价 C++ 的栈对象 `JZPainter p(this)` —— 只在本帧绘制派发内有效。
//    C++ 靠作用域自动析构(构造写 Push、析构写 Pop 因此天然配对); JS 没有栈作用域, 只能等 GC,
//    而绘制流每帧回放后就被清空(JZGdiBackend::render → layer->clearTasks), 晚到的 Pop 会落到下一帧
//    破坏配对(同帧 GC 还会插在其它控件的 Push/Pop 之间)。故这里用 Guard 界定作用域:
//    派发内 new 出来的画笔登记在册, 派发结束立即回收 —— 析构与 Push 同帧配平; 同时让 JS 侧包装失效,
//    离开 onPaint 后再调用抛"指针已被释放"(与 C++ 栈对象"出作用域就不能用"一致)。
// 2) 离屏画笔 `new JZPainter(image)`: 自持图层, 析构时把本帧绘制合成回 image —— 不受派发约束,
//    可随时创建(如启动时生成图标)。随 JS GC 释放; 想让 image 立刻可用则显式调 `p.end()`
//    (析构时机由 GC 决定, 不调 end() 时"何时写回 image"是不可预期的)。
//
// ---- 登记/失效靠"共享状态", 不靠 JS 对象 ----
// 创建时只留下包装的 `JSRawPointer<JZPainter>::Info`(shared PointInfo, 与 JS 侧那份共享), 因此:
//   * 不需要 setInstanceBind/onCreated 之类的绑定钩子(createArgs 阶段拿不到 JSValue, 本来也不必拿);
//   * 不需要 dup JS 对象保活 —— JS 对象中途被 GC 也不影响: 状态独立存活, 标记 cleared 后
//     JS 侧(若还在)立即失效,"另外一边"不会悬垂。
// 屏幕画笔: Info 留在帧表里, 派发结束 `delete 画笔 + Info->invalidate()`。
// 离屏画笔: Info + 目标 image 的 JS 引用留在离屏表里(保证画笔析构回写时 image 还活着),
//           JS 对象被回收 / 显式 end() / 模块退出时结清。
class JZJSPaintScope
{
public:
    JZJSPaintScope() = default;
    ~JZJSPaintScope();

    JZJSPaintScope(const JZJSPaintScope&) = delete;
    JZJSPaintScope& operator=(const JZJSPaintScope&) = delete;

    // 引擎绑定(initEngine 时设置; 取 JSContext 用于释放离屏表里的目标引用)
    void setEngine(JZJSEngine* engine) { m_engine = engine; }
    // 引擎退出(deinitEngine): 回收帧内画笔 + 结清离屏表, 并断开引擎引用
    void reset();

    // 绘制派发守卫(RAII): JSWidgetClass::paintEvent 里构造, 出作用域回收本次派发内 new 的画笔。
    // 每层派发一个 frame, 支持嵌套(onPaint 里触发同步重绘)。
    class Guard
    {
    public:
        explicit Guard(JZJSPaintScope* scope);
        ~Guard();

        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        JZJSPaintScope* m_scope = nullptr;
    };

    // JS `new JZPainter(target)` 的动态转发(按实参类型选构造函数 + 定所有权 + 登记生命周期):
    //   target = JZWidget -> 屏幕画笔, 两条硬约束:
    //        ① 只能在 onPaint 回调内创建(即绘制派发期间) —— 画笔只对这一次绘制有意义;
    //        ② 同一次绘制内**每个控件只能有一份** —— 画笔自带 Push/Pop 与裁剪状态, 同控件多份会重复
    //           叠加/状态交叉。违反任一条直接抛错(不静默返回)。
    //        **cppOwner=true**, C++ 拥有 —— 派发结束由本作用域 delete 并让 JS 句柄失效。
    //   target = JZImage  -> 离屏画笔(自持图层, 析构/end() 时合成回 image; cppOwner=false, JS 拥有),
    //                        不受上述两条约束(不碰窗口绘制流)。
    // 返回统一包装 JSRawPointer<JZPainter>*(createValue 的 opaque 约定),
    // 不能直接返回底层裸指针 —— 否则后续按 JSPointerBase* 访问是 UB。
    jsbind::JSRawPointer<JZPainter>* createPainter(JSContext* ctx, int argc, JSValueConst* argv);

    // JS 侧 `p.end()`: 立刻结束绘制 —— 离屏模式把图层合成回 image(做完 image 才可用),
    // 屏幕模式提前补 Pop; 之后该包装失效。传的是 JS 对象内部的包装引用, 改动会传回 JS 那份。
    void endPainter(jsbind::JSRawPointer<JZPainter>& p);

private:
    using Info = jsbind::JSRawPointer<JZPainter>::Info;

    struct FrameEntry
    {
        JZPainter* painter = nullptr;
        Info info;             // 与 JS 侧包装共享的句柄状态(状态独立存活, 不依赖包装还活着)
        JZWidget* widget = nullptr;   // 目标控件(判"同一次绘制每个控件只能一份"用)
    };

    struct OffscreenEntry
    {
        Info info;
        JSValue target = JS_UNDEFINED;   // 目标 image 的 JS 对象(持一份强引用)
    };

    void begin();
    void end();

    // 离屏表结清: JS 对象已被回收(只剩本表持有 Info)的项 -> 放掉 Info(触发画笔析构回写)后再放目标引用。
    void sweepOffscreen();
    void releaseOffscreen(const void* info);

    JZJSEngine* m_engine = nullptr;
    std::vector<std::vector<FrameEntry>> m_frames;   // 每层派发一个 frame
    std::vector<OffscreenEntry> m_offscreen;
};

#endif // !JZ_JS_PAINT_SCOPE_H_
