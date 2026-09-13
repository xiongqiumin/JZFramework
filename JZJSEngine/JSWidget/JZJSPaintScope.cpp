#include "JZJSPaintScope.h"

#include <assert.h>

#include "JZJSEngine.h"
#include "JZPainter.h"

// 绘制派发可能嵌套(onPaint 里触发 update/repaint 会同步重绘), 故用栈记录当前作用域。
static std::vector<JZJSPaintScope*>& jzPaintScopeStack()
{
    static std::vector<JZJSPaintScope*> s_stack;
    return s_stack;
}

JZJSPaintScope* JZJSPaintScope::current()
{
    std::vector<JZJSPaintScope*>& s = jzPaintScopeStack();
    return s.empty() ? nullptr : s.back();
}

JZJSPaintScope::JZJSPaintScope(JZJSEngine* engine)
    : m_engine(engine)
{
    jzPaintScopeStack().push_back(this);
}

JZJSPaintScope::~JZJSPaintScope()
{
    std::vector<JZJSPaintScope*>& s = jzPaintScopeStack();
    assert(!s.empty() && s.back() == this);
    s.pop_back();

    JSContext* ctx = m_engine ? m_engine->context() : nullptr;

    // 立刻回收本次派发内 new 的画笔:
    //   1) 先翻成 C++ 拥有 —— 否则包装析构时还会按 PointInfo 的 cppOwner(false) 再 delete 一次;
    //   2) delete 画笔, 使析构写入的 Pop 与本次派发的 Push 在同一帧内配平;
    //   3) 让 JS 侧那份包装失效(共享 PointInfo, 之后的绘制调用抛"指针已被释放");
    //   4) 放掉作用域持有的 JS 引用; 包装本身仍由 JS 对象持有, 随其 GC 经 finalizer 释放。
    for (Entry& e : m_entries)
    {
        if (e.wrapper)
        {
            JZPainter* painter = e.wrapper->get();
            e.wrapper->setCppOwner(true);
            delete painter;
            e.wrapper->clear();
        }
        if (ctx && !JS_IsUndefined(e.obj))
            JS_FreeValue(ctx, e.obj);
    }
    m_entries.clear();
    m_markNext = false;
}

bool JZJSPaintScope::active()
{
    return current() != nullptr;
}

void JZJSPaintScope::markJsCreated()
{
    JZJSPaintScope* scope = current();
    if (scope)
        scope->m_markNext = true;
}

bool JZJSPaintScope::track(JSContext* ctx, jsbind::JSRawPointer<JZPainter>* wrapper, JSValue obj)
{
    JZJSPaintScope* scope = current();
    if (!scope || !scope->m_markNext)
        return false;
    scope->m_markNext = false;

    Entry e;
    e.wrapper = wrapper;
    e.obj = JS_DupValue(ctx, obj);   // 保活: 包装即 JS 对象的 opaque, 必须活到回收那一刻
    scope->m_entries.push_back(e);
    return true;
}
