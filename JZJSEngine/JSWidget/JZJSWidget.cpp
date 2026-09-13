#include "JZJSWidget.h"

#include "JZJSEngine.h"
#include "JZLayout.h"
#include "JZWindow.h"

JZ_OBJECT_IMPL(JZJSWidget, JZWidget)

namespace
{
// JS 指针包装 -> 其指向的 JZWidget; 包装为空(null 或已失效)时返回 nullptr。
JZWidget* widgetOf(const jsbind::JSObjectPointer& ref)
{
    return jzobject_cast<JZWidget*>(ref.get());
}
}

// ---------- 实例回写 ----------

void JZJSWidget::setInstance(JZJSEngine* engine, JSValue jsvalue)
{
    m_engine = engine;
    m_jsvalue = jsvalue;   // 不 dup: 随 JS 对象自身 GC
}

jsbind::JSPointerBase* JZJSWidget::selfPointer() const
{
    if (!m_engine || JS_IsUndefined(m_jsvalue))
        return nullptr;

    JSClassID class_id = 0;
    void* opaque = JS_GetAnyOpaque(m_jsvalue, &class_id);
    if (!opaque || !m_engine->isJSRawPointerClassId(class_id))
        return nullptr;

    return (jsbind::JSPointerBase*)opaque;
}

// ---------- 父控件 ----------

void JZJSWidget::setParentWidgetJs(const jsbind::JSObjectPointer& parent)
{
    JZWidget* p = widgetOf(parent);

    setParentWidget(p);

    // 状态转移(/TransferThis/): 挂进对象树后由父(对象树)负责 delete -> 翻转成非拥有;
    // 脱离对象树(parent 为空)则交回 JS 拥有。翻转的是本对象自己的包装, 共享 PointInfo,
    // 所以 JS 侧那份也同步生效。纯 C++ 创建(无 JS 包装)时 selfPointer() 为 nullptr, 无需处理。
    jsbind::JSPointerBase* self = selfPointer();
    if (self)
        self->setCppOwner(p != nullptr);
}

jsbind::JSObjectPointer JZJSWidget::parentWidgetJs() const
{
    // 借用: 父控件销毁后守卫指针自动置空, 无需缓存或通知。
    return jsbind::JSObjectPointer(parentWidget(), true);
}

// ---------- 坐标映射 ----------

JZPoint JZJSWidget::mapToJs(const jsbind::JSObjectPointer& parent, const JZPoint& pt) const
{
    return mapTo(widgetOf(parent), pt);
}

// ---------- 窗口 ----------

jsbind::JSObjectPointer JZJSWidget::windowJs() const
{
    return jsbind::JSObjectPointer(window(), true);
}

// ---------- 布局 ----------

jsbind::JSRawPointer<JZLayout> JZJSWidget::layoutJs() const
{
    revalidateLayoutJs(m_layout);
    return m_layoutJs;
}

void JZJSWidget::setLayoutJs(jsbind::JSRawPointer<JZLayout>& layout)
{
    JZLayout* lay = layout.get();

    // /Transfer/: JS 侧对象交给本控件接管, 之后由 C++ 负责 delete。
    // setCppOwner 改的是共享的 PointInfo, 所以 JS 对象自己那份包装也被翻转 ——
    // 之后 JS GC 不会再重复释放。
    layout.setCppOwner(true);

    // 基类 setLayout 会 delete 旧布局, 因此这里必须**主动**失效(不能等惰性重验证),
    // 否则已经发给 JS 的旧包装会悬垂。顺带直接换成新布局的视图。
    m_layoutJs.clear();
    m_layoutJs = jsbind::JSRawPointer<JZLayout>(lay, true);

    JZWidget::setLayout(lay);
}

void JZJSWidget::revalidateLayoutJs(JZLayout* cur) const
{
    if (m_layoutJs.get() == cur && m_layoutJs.cppOwner())
        return;

    // 只比较指针值、不解引用, 因此即使 cur 指向已被 delete 的旧布局也安全。
    m_layoutJs.clear();                                      // 旧包装(含已发给 JS 的副本)一并失效
    m_layoutJs = jsbind::JSRawPointer<JZLayout>(cur, true);  // cppOwner=true: 非拥有视图
}
