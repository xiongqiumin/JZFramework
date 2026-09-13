#ifndef JZ_JS_WIDGET_H_
#define JZ_JS_WIDGET_H_

#include "JZWidget.h"
#include "JZJSType.h"
#include "JZJSWidgetModule.h"   // JSWidgetClass<Class> (逐虚函数转发: paintEvent / mouseXxx / ...)

class JZJSEngine;
class JZLayout;

// JZWidget 的 JS 侧镜像类。两层职责:
//   1) 事件转发 —— 继承 JSWidgetClass<JZWidget>, 逐虚函数把 paintEvent / mouseXxxEvent /
//      keyXxxEvent / resizeEvent ... 转给 JS 的 onPaint / onMousePress ... (见 JZJSWidgetModule.h)。
//      虚函数签名保持与 JZWidget 完全一致(如 paintEvent(JZPainterEvent*)), 只是多了"有 onXxx 就转 JS"。
//   2) 指针接口 —— 把 JZWidget 中"含指针"的接口用 JS 指针包装类型重新表达一遍。
//
// 命名约定 (JZJS 层统一):
//   凡签名中出现 jsbind::JSObjectPointer / jsbind::JSRawPointer 的方法, 一律以 Js 结尾。
//   原因: C++ 不能只按返回类型重载, 所以 layout() / parentWidget() / window() 这类无参
//   getter 只能换名; setter 一并跟随, 保持整套风格统一。
//   **JS 侧看到的方法名不带后缀**, 由绑定层 def("layout", &JZJSWidget::layoutJs) 决定,
//   后缀只是 C++ 侧的区分手段。
//
// 两类包装的选用依据 (详见 JZJSType.h):
//   JZObject 派生类(父控件 / 窗口) -> JSObjectPointer: 守卫指针自动失效, 无需缓存;
//   非 JZObject 类型(JZLayout)     -> JSRawPointer<T>: 无守卫, 需缓存一份作"失效开关"。
class JZJSWidget : public JSWidgetClass<JZWidget>
{
    JZ_OBJECT_DECLARE(JZJSWidget)

public:
    // 绑定层在实例创建后回写(ClassBind::setInstanceBind 的 onCreated 钩子)。
    // 成员名与 JSWidgetClass 保持一致, 故 jzBindWidgetInstance<JZJSWidget> 那类钩子可直接复用。
    // m_jsvalue 不 dup: 随 JS 对象自身 GC(与 JSWidgetClass 同一约定)。
    void setInstance(JZJSEngine* engine, JSValue jsvalue);

    // ---- 父控件 (转移所有权; JZObject 派生 -> JSObjectPointer) ----
    // 状态转移(/TransferThis/): setParentWidget 会把本控件挂进父对象的对象树, 此后由**父(对象树)**
    // 负责 delete, 因此必须把"本控件自己的 JS 包装"翻转为非拥有 —— 否则 JS GC 会与对象树双重释放。
    // 反向(脱离对象树, parent 为空)则交回 JS 拥有。
    // 注: 翻转的是接收者(this)自己的包装, C++ 侧经 m_jsvalue 反查得到(见 selfPointer())。
    void setParentWidgetJs(const jsbind::JSObjectPointer& parent);
    jsbind::JSObjectPointer parentWidgetJs() const;

    // ---- 坐标映射 ----
    // parent 为空(null 或已销毁)时映射到屏幕坐标, 与基类语义一致。
    JZPoint mapToJs(const jsbind::JSObjectPointer& parent, const JZPoint& pt) const;

    // ---- 所属窗口 (借用; JZObject 派生 -> JSObjectPointer) ----
    jsbind::JSObjectPointer windowJs() const;

    // ---- 布局 (非 JZObject -> JSRawPointer + 惰性重验证) ----
    // 基类 m_layout 由控件拥有(析构 / setLayout 时 delete); 这里返回的是**非拥有**视图。
    // m_layoutJs 缓存在本对象上, 充当"失效开关": 布局被替换或删除时, 已经发给 JS 的旧包装
    // 会一起失效(它们与本缓存共享同一个 PointInfo)。
    jsbind::JSRawPointer<JZLayout> layoutJs() const;

    // 交出布局所有权: JS 创建的布局被本控件接管(/Transfer/), 之后由 C++ 负责 delete,
    // JS 侧那份包装会被翻转为"非拥有"(共享 PointInfo), GC 时不再重复释放。
    void setLayoutJs(jsbind::JSRawPointer<JZLayout>& layout);

protected:
    // 惰性重验证: C++ 侧真实布局与本缓存不一致(被替换 / 删除)时重建, 并让旧包装失效。
    // 只在"下次访问 layoutJs()"时生效; 删除点(setLayoutJs)会主动失效, 以免悬垂。
    void revalidateLayoutJs(JZLayout* cur) const;

    // 本对象自己的 JS 包装(即 JS 对象的 opaque)。
    // C++ 成员函数拿不到"this 的 JS 包装", 故经 m_jsvalue 反查: JS_GetAnyOpaque 取 opaque,
    // 再用引擎确认它是注册过的包装类。无 JS 包装(纯 C++ 创建的对象)时返回 nullptr,
    // 此时所有权转移无需处理。
    jsbind::JSPointerBase* selfPointer() const;

    // mutable: layoutJs() 是 const 方法, 但惰性重验证需要写入本缓存。
    mutable jsbind::JSRawPointer<JZLayout> m_layoutJs;

    // 注: m_engine / m_jsvalue 由基类 JSWidgetClass 提供(绑定层经 setInstance 回写),
    // 这里不再重复声明 —— 否则派生类的同名成员会遮蔽基类的, 事件转发将读不到 JS 侧对象。
};

#endif // !JZ_JS_WIDGET_H_
