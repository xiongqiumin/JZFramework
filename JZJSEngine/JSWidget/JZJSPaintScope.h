#ifndef JZ_JS_PAINT_SCOPE_H_
#define JZ_JS_PAINT_SCOPE_H_

#include <vector>

#include "quickjs/quickjs.h"
#include "JZJSType.h"

class JZJSEngine;
class JZPainter;

// JS 侧 JZPainter 的生命周期作用域 —— 对应 C++ 里 "画笔是 paintEvent 的栈对象"。
//
// C++ 写法 `JZPainter p(this);` 出作用域即析构, 构造时写入绘制流的 Push 与析构写入的 Pop
// 因此天然配对。JS 没有栈作用域, `new JZPainter(this)` 只能等 GC 回收, 而绘制流每帧回放后就被
// 清空 (JZGdiBackend::render → layer->clearTasks), 晚到的 Pop 会落到下一帧的绘制流里,
// 把别人(甚至别的控件)Push 的状态弹掉 —— 同帧内 GC 触发还会插在其它控件的 Push/Pop 之间。
//
// 所以: onPaint 派发期间 new 出来的 painter 全部登记在此, 派发结束立刻回收 ——
//   1) 析构时写入的 Pop 与本次派发的 Push 同帧配平;
//   2) 同时让 JS 侧那份包装失效, 离开 onPaint 后再调用 drawXxx 抛 "指针已被释放",
//      而不是把绘制命令悄悄混进后续帧 (与 C++ 栈对象"出了作用域就不能用"语义一致)。
//
// 用点: JSWidgetClass::paintEvent (开作用域) 与 JZJSWidgetModule::registPainter (登记/回收)。
class JZJSPaintScope
{
public:
    explicit JZJSPaintScope(JZJSEngine* engine);
    ~JZJSPaintScope();

    JZJSPaintScope(const JZJSPaintScope&) = delete;
    JZJSPaintScope& operator=(const JZJSPaintScope&) = delete;

    // 是否处于绘制派发内(用于校验"画笔只能在 onPaint 里 new", 与 C++ 契约一致)。
    static bool active();

    // createArgs 中调用: 声明"紧接着 createValue 包出来的这个 painter 是 JS new 的",
    // 需要由本作用域接管回收(区别于 C++ 侧交出来的借用画笔, 那种不能删)。
    static void markJsCreated();

    // onCreated 钩子中调用: 登记 markJsCreated 标记过的包装并保活到作用域结束。
    // 返回 true = 已登记(回收由作用域负责); false = 不在绘制派发内, 由 GC 负责。
    static bool track(JSContext* ctx, jsbind::JSRawPointer<JZPainter>* wrapper, JSValue obj);

private:
    static JZJSPaintScope* current();

    struct Entry
    {
        jsbind::JSRawPointer<JZPainter>* wrapper = nullptr;
        JSValue obj = JS_UNDEFINED;
    };

    JZJSEngine* m_engine = nullptr;
    bool m_markNext = false;   // markJsCreated 置位, track 消费
    std::vector<Entry> m_entries;
};

#endif // !JZ_JS_PAINT_SCOPE_H_
