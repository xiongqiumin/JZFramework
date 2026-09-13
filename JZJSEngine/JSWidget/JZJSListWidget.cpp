#include "JZJSListWidget.h"

JZ_OBJECT_IMPL(JZJSListWidget, JZListWidget)

JZJSListWidget::JZJSListWidget(JZWidget* parent)
    : JSWidgetClass<JZListWidget>(parent)
{
}

JZJSListWidget::~JZJSListWidget()
{
    // 列表销毁 -> 所有项随之释放, 已发给 JS 的项包装必须先失效(借用型全部 clear)。
    invalidateAllItemJs();
}

// ---------- 失效开关表 ----------

jsbind::JSRawPointer<JZListWidgetItem>& JZJSListWidget::switchFor(JZListWidgetItem* item) const
{
    for (size_t i = 0; i < m_itemJs.size(); i++)
    {
        if (m_itemJs[i].item == item)
        {
            if (m_itemJs[i].sw.get() != item)   // 曾被 clear(如 take 后又放回) -> 重建为借用
                m_itemJs[i].sw = jsbind::JSRawPointer<JZListWidgetItem>(item, true);
            return m_itemJs[i].sw;
        }
    }

    m_itemJs.push_back(ItemSwitch());
    m_itemJs.back().item = item;
    m_itemJs.back().sw = jsbind::JSRawPointer<JZListWidgetItem>(item, true);   // 借用
    return m_itemJs.back().sw;
}

jsbind::JSRawPointer<JZListWidgetItem> JZJSListWidget::adoptSwitch(
    JZListWidgetItem* item, const jsbind::JSRawPointer<JZListWidgetItem>& src) const
{
    for (size_t i = 0; i < m_itemJs.size(); i++)
    {
        if (m_itemJs[i].item == item)
        {
            m_itemJs[i].sw = src;               // 共享 src 的 PointInfo
            return m_itemJs[i].sw;
        }
    }

    m_itemJs.push_back(ItemSwitch());
    m_itemJs.back().item = item;
    m_itemJs.back().sw = src;
    return m_itemJs.back().sw;
}

void JZJSListWidget::invalidateItemJs(JZListWidgetItem* item) const
{
    for (size_t i = 0; i < m_itemJs.size(); i++)
        if (m_itemJs[i].item == item)
            m_itemJs[i].sw.clear();             // 共享 PointInfo -> 已发给 JS 的副本一起失效
}

void JZJSListWidget::invalidateAllItemJs() const
{
    for (size_t i = 0; i < m_itemJs.size(); i++)
        m_itemJs[i].sw.clear();
    m_itemJs.clear();
}

void JZJSListWidget::pruneItemJs() const
{
    for (size_t i = 0; i < m_itemJs.size(); )
    {
        JZListWidgetItem* it = m_itemJs[i].item;
        if (row(it) >= 0)
        {
            i++;
            continue;
        }

        // 已不在列表中:
        //   借用(cppOwner=true) —— 说明是列表自己拥有的项被删了, 兜底失效(防止悬垂);
        //   非拥有(false)      —— 是 takeItemJs 交回 JS 的正常结果, 只丢开关、不能失效。
        if (m_itemJs[i].sw.cppOwner())
            m_itemJs[i].sw.clear();

        m_itemJs.erase(m_itemJs.begin() + i);
    }
}

// ---------- 查询 ----------

jsbind::JSRawPointer<JZListWidgetItem> JZJSListWidget::itemJs(int index) const
{
    JZListWidgetItem* it = item(index);
    if (!it)
        return jsbind::JSRawPointer<JZListWidgetItem>();

    pruneItemJs();                              // C++ 侧可能直接改过列表, 先剔除失效项
    return switchFor(it);                       // 拷贝返回, 与开关共享 PointInfo
}

jsbind::JSRawPointer<JZListWidgetItem> JZJSListWidget::currentItemJs() const
{
    return itemJs(currentIndex());
}

// ---------- 追加 / 插入 ----------

jsbind::JSRawPointer<JZListWidgetItem> JZJSListWidget::appendItemJs(const std::string& text)
{
    // 基类接口不返回句柄(与 Qt 一致), 所以这里自己 new 再交给基类接管。
    JZListWidgetItem* it = new JZListWidgetItem(text);
    JZListWidget::appendItem(it);
    return switchFor(it);
}

jsbind::JSRawPointer<JZListWidgetItem> JZJSListWidget::insertItemJs(int index, const std::string& text)
{
    JZListWidgetItem* it = new JZListWidgetItem(text);
    JZListWidget::insertItem(index, it);
    return switchFor(it);                       // 下标错位不影响: 开关按项指针索引
}

jsbind::JSRawPointer<JZListWidgetItem> JZJSListWidget::appendItemJs(
    jsbind::JSRawPointer<JZListWidgetItem>& item)
{
    JZListWidgetItem* raw = item.get();
    if (!raw)
        return jsbind::JSRawPointer<JZListWidgetItem>();

    // /Transfer/: 所有权交给本列表(基类的 appendItem(指针) 会用 unique_ptr 接管),
    // 翻转后 JS 侧那份包装变成"非拥有", GC 时不会再重复释放。
    item.setCppOwner(true);

    JZListWidget::appendItem(raw);

    // 用 JS 侧那份包装直接当开关(共享 PointInfo): 之后本列表删除该项时, 失效能传回 JS。
    return adoptSwitch(raw, item);
}

jsbind::JSRawPointer<JZListWidgetItem> JZJSListWidget::insertItemJs(
    int index, jsbind::JSRawPointer<JZListWidgetItem>& item)
{
    JZListWidgetItem* raw = item.get();
    if (!raw)
        return jsbind::JSRawPointer<JZListWidgetItem>();

    item.setCppOwner(true);                                  // /Transfer/

    JZListWidget::insertItem(index, raw);

    return adoptSwitch(raw, item);
}

// ---------- 摘除 ----------

void JZJSListWidget::removeItemJs(int index)
{
    JZListWidgetItem* it = item(index);
    if (!it)
        return;

    invalidateItemJs(it);                       // 下面就会 delete 该项, 先让 JS 侧包装失效
    delete JZListWidget::takeItem(index);       // Qt 惯用法: 摘除并删除
}

jsbind::JSRawPointer<JZListWidgetItem> JZJSListWidget::takeItemJs(int index)
{
    JZListWidgetItem* it = item(index);         // 必须在 take 之前取(之后下标就变了)
    if (!it)
        return jsbind::JSRawPointer<JZListWidgetItem>();

    JZListWidget::takeItem(index);              // 摘除, 所有权交回调用方

    // /TransferBack/: 交回 JS 拥有。复用同一个开关(共享 PointInfo), 因此 JS 侧原先持有的
    // 那份借用包装会一起变成"拥有者" —— 它们共享一个 PointInfo, 只会释放一次。
    jsbind::JSRawPointer<JZListWidgetItem>& sw = switchFor(it);
    sw.setCppOwner(false);
    return sw;
}

void JZJSListWidget::clearJs()
{
    invalidateAllItemJs();                      // 基类 clear() 会 delete 全部项, 先失效
    JZListWidget::clear();
}
