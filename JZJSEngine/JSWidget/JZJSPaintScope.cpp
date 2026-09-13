#include "JZJSPaintScope.h"

#include <typeinfo>

#include "JZJSEngine.h"
#include "JZImage.h"

namespace
{
// 实参是否是 JZImage(动态转发用: 按目标类型决定走离屏还是屏幕构造函数)。
// 走注册表 ctypeid + super 链判定, 与 fromJSValue 的类型判定同一套。
bool jsArgIsImage(JSContext* ctx, JSValueConst value)
{
    if (JS_IsNull(value) || JS_IsUndefined(value))
        return false;

    auto* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    if (!engine)
        return false;

    JSClassID id = 0;
    void* opaque = JS_GetAnyOpaque(value, &id);
    if (!opaque)
        return false;

    return engine->isInstanceOf(engine->classCTypeId(id), typeid(JZImage).name());
}
}   // namespace

// ---------------- 生命周期 ----------------

JZJSPaintScope::~JZJSPaintScope()
{
    reset();
}

void JZJSPaintScope::reset()
{
    while (!m_frames.empty())
        end();

    JSContext* ctx = m_engine ? m_engine->context() : nullptr;
    for (OffscreenEntry& e : m_offscreen)
    {
        e.info.reset();   // 触发画笔析构(回写 image) —— 目标引用此时还没放, 仍有效
        if (ctx && !JS_IsUndefined(e.target))
            JS_FreeValue(ctx, e.target);
    }
    m_offscreen.clear();
    m_engine = nullptr;
}

// ---------------- 绘制派发作用域 ----------------

JZJSPaintScope::Guard::Guard(JZJSPaintScope* scope)
    : m_scope(scope)
{
    if (m_scope)
        m_scope->begin();
}

JZJSPaintScope::Guard::~Guard()
{
    if (m_scope)
        m_scope->end();
}

void JZJSPaintScope::begin()
{
    sweepOffscreen();
    m_frames.push_back(std::vector<FrameEntry>());
}

void JZJSPaintScope::end()
{
    if (m_frames.empty())
        return;

    std::vector<FrameEntry> frame = std::move(m_frames.back());
    m_frames.pop_back();

    // 立即回收本次派发内 new 出来的屏幕画笔:
    //   1) delete 画笔: 让析构写入的 Pop 与本次派发的 Push 在同一帧内配平;
    //   2) 标记共享状态失效 —— JS 侧那份包装(若还在)立即失效, 之后调用抛"指针已被释放";
    //      状态是 shared_ptr, 即使 JS 对象中途已被 GC 也照样安全(不存在悬垂指针)。
    // 已由 JS 侧 p.end() 提前结束的项: 画笔那时已被 delete、状态已标记失效, 这里必须跳过
    // (状态里的 cleared 就是"画笔已不在"的判据), 否则会重复 delete。
    for (FrameEntry& e : frame)
    {
        if (!e.info || e.info->cleared)
            continue;
        delete e.painter;
        e.info->invalidate();
    }
}

// ---------------- 创建分发(动态转发) ----------------

jsbind::JSRawPointer<JZPainter>* JZJSPaintScope::createPainter(JSContext* ctx, int argc, JSValueConst* argv)
{
    // createArgs 约定: 参数不合法时"设 JS 异常 + 返回 nullptr"(不抛 C++ 异常),
    // JZJSEngine::createValue 见到空指针就把该异常交给 JS。
    if (argc < 1)
    {
        JS_ThrowTypeError(ctx, "new JZPainter(target): 需要 JZWidget 或 JZImage 参数");
        return nullptr;
    }

    // 离屏绘制: 目标是图像 —— JZPainter 自持图层, 析构时合成回 image, 不受绘制派发约束。
    if (jsArgIsImage(ctx, argv[0]))   // 校验在前(构造参数没有类型表, 得自己先校验)
    {
        JZImage* img = fromJSValue<JZImage*>(ctx, argv[0]);
        if (img->width() <= 0 || img->height() <= 0)
        {
            JS_ThrowTypeError(ctx, "new JZPainter(image): image 尺寸无效(请先用 JZImage::create 建立缓冲区)");
            return nullptr;
        }

        auto* wrapper = new jsbind::JSRawPointer<JZPainter>(new JZPainter(*img), false);   // JS 拥有
        // 登记: 目标 image 持一份强引用, 保证画笔析构回写时它还没被 GC。
        sweepOffscreen();
        OffscreenEntry e;
        e.info = wrapper->info();
        e.target = JS_DupValue(ctx, argv[0]);
        m_offscreen.push_back(e);
        return wrapper;
    }

    // 屏幕绘制: 目标是控件 —— 画笔只在本次绘制派发内有效(同 C++ 的 `JZPainter p(this)`)。
    // 所有权也归 C++(本作用域): 创建即 cppOwner=true, JS 只持句柄 —— 派发结束由本作用域 delete
    // 并让句柄失效; JS 侧提前 GC 只是丢句柄(状态独立存活), 不会误删也不会悬垂。
    // 约束① 只能在绘制派发内创建。
    if (m_frames.empty())
    {
        JS_ThrowTypeError(ctx, "new JZPainter(widget): 只能在 onPaint 回调内创建(画笔仅本次绘制有效)");
        return nullptr;
    }

    if (!js_check_class_arg<JZWidget>(ctx, argv[0]))
    {
        JS_ThrowTypeError(ctx, "new JZPainter(target): 需要控件(JZWidget)或图像(JZImage)参数");
        return nullptr;
    }
    JZWidget* w = fromJSValue<JZWidget*>(ctx, argv[0]);

    // 约束② 同一次绘制内每个控件只能有一份画笔(画笔携带 Push/Pop 与裁剪状态, 多份会重复叠加)。
    std::vector<FrameEntry>& frame = m_frames.back();
    for (size_t i = 0; i < frame.size(); i++)
    {
        if (frame[i].widget == w)
        {
            JS_ThrowTypeError(ctx, "new JZPainter(widget): 该控件本次绘制已创建过画笔(每帧每控件只能一份)");
            return nullptr;
        }
    }

    auto* wrapper = new jsbind::JSRawPointer<JZPainter>(new JZPainter(w), true);

    FrameEntry e;
    e.painter = wrapper->get();
    e.info = wrapper->info();   // 只留共享状态: 之后释放不需要 JS 对象, 也不需要包装本身还活着
    e.widget = w;
    frame.push_back(e);
    return wrapper;
}

// ---------------- 显式结束 ----------------

void JZJSPaintScope::endPainter(jsbind::JSRawPointer<JZPainter>& p)
{
    JZPainter* raw = p.get();
    if (!raw)
        return;

    Info info = p.info();

    p.setCppOwner(true);   // 防包装析构时重复 delete
    delete raw;            // 析构: 离屏回写 image / 屏幕补 Pop
    p.clear();             // JS 侧那份(共享同一点状态)一起失效

    releaseOffscreen(info.get());   // 离屏画笔已结束, 不必再保活它的目标
}

// ---------------- 离屏表 ----------------

void JZJSPaintScope::sweepOffscreen()
{
    JSContext* ctx = m_engine ? m_engine->context() : nullptr;
    for (size_t i = 0; i < m_offscreen.size(); )
    {
        OffscreenEntry& e = m_offscreen[i];
        // use_count()==1 表示只剩本表持有该状态 -> JS 侧包装已被回收(画笔尚未析构, 因为它由状态析构触发)。
        if (!e.info || e.info.use_count() == 1)
        {
            e.info.reset();   // 触发 PointInfo 析构 -> delete 画笔 -> 回写 image(此时 image 还在, 见下一行)
            if (ctx && !JS_IsUndefined(e.target))
                JS_FreeValue(ctx, e.target);
            m_offscreen.erase(m_offscreen.begin() + i);
            continue;
        }
        ++i;
    }
}

void JZJSPaintScope::releaseOffscreen(const void* info)
{
    JSContext* ctx = m_engine ? m_engine->context() : nullptr;
    for (size_t i = 0; i < m_offscreen.size(); i++)
    {
        if (m_offscreen[i].info.get() != info)
            continue;

        m_offscreen[i].info.reset();
        if (ctx && !JS_IsUndefined(m_offscreen[i].target))
            JS_FreeValue(ctx, m_offscreen[i].target);
        m_offscreen.erase(m_offscreen.begin() + i);
        return;
    }
}
