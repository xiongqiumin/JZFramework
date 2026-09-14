#include "JZJSTableWidget.h"

JZ_OBJECT_IMPL(JZJSTableWidget, JZTableWidget)

JZJSTableWidget::JZJSTableWidget(JZWidget* parent)
    : JZTableWidget(parent)
{
}

JZJSTableWidget::~JZJSTableWidget()
{
    // 表格销毁 -> 所有单元格随之释放, 已发给 JS 的包装必须先失效(借用型全部 clear)。
    invalidateAllItemJs();
}

// ---------- 失效开关表 ----------

jsbind::JSRawPointer<JZTableWidgetItem>& JZJSTableWidget::switchFor(JZTableWidgetItem* item) const
{
    for (size_t i = 0; i < m_itemJs.size(); i++)
    {
        if (m_itemJs[i].item == item)
        {
            if (m_itemJs[i].sw.get() != item)   // 曾被 clear(如 take 后又放回) -> 重建为借用
                m_itemJs[i].sw = jsbind::JSRawPointer<JZTableWidgetItem>(item, true);
            return m_itemJs[i].sw;
        }
    }

    m_itemJs.push_back(ItemSwitch());
    m_itemJs.back().item = item;
    m_itemJs.back().sw = jsbind::JSRawPointer<JZTableWidgetItem>(item, true);   // 借用
    return m_itemJs.back().sw;
}

jsbind::JSRawPointer<JZTableWidgetItem> JZJSTableWidget::adoptSwitch(
    JZTableWidgetItem* item, const jsbind::JSRawPointer<JZTableWidgetItem>& src) const
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

void JZJSTableWidget::invalidateItemJs(JZTableWidgetItem* item) const
{
    if (!item)
        return;

    for (size_t i = 0; i < m_itemJs.size(); i++)
        if (m_itemJs[i].item == item)
            m_itemJs[i].sw.clear();             // 共享 PointInfo -> 已发给 JS 的副本一起失效
}

void JZJSTableWidget::invalidateAllItemJs() const
{
    for (size_t i = 0; i < m_itemJs.size(); i++)
        m_itemJs[i].sw.clear();
    m_itemJs.clear();
}

void JZJSTableWidget::pruneItemJs() const
{
    for (size_t i = 0; i < m_itemJs.size(); )
    {
        JZTableWidgetItem* it = m_itemJs[i].item;
        if (row(it) >= 0)
        {
            i++;
            continue;
        }

        // 已不在表格中:
        //   借用(cppOwner=true) —— 说明是表格自己拥有的 item 被删了, 兜底失效(防止悬垂);
        //   非拥有(false)      —— 是 takeItemJs 交回 JS 的正常结果, 只丢开关、不能失效。
        if (m_itemJs[i].sw.cppOwner())
            m_itemJs[i].sw.clear();

        m_itemJs.erase(m_itemJs.begin() + i);
    }
}

// ---------- 查询 ----------

jsbind::JSRawPointer<JZTableWidgetItem> JZJSTableWidget::itemJs(int row, int col) const
{
    JZTableWidgetItem* it = item(row, col);
    if (!it)
        return jsbind::JSRawPointer<JZTableWidgetItem>();

    pruneItemJs();                              // C++ 侧可能直接改过表格, 先剔除失效项
    return switchFor(it);                       // 拷贝返回, 与开关共享 PointInfo
}

jsbind::JSRawPointer<JZTableWidgetItem> JZJSTableWidget::currentItemJs() const
{
    return itemJs(currentRow(), currentColumn());
}

// ---------- 落位 / 接管 ----------

jsbind::JSRawPointer<JZTableWidgetItem> JZJSTableWidget::setItemJs(
    int row, int col, jsbind::JSRawPointer<JZTableWidgetItem>& item)
{
    JZTableWidgetItem* raw = item.get();
    if (!raw)
    {
        clearItemJs(row, col);
        return jsbind::JSRawPointer<JZTableWidgetItem>();
    }

    // 本格原有 item 会被顶掉(删除): 先让它的 JS 包装失效, 否则 JS 侧会拿到悬垂句柄
    invalidateItemJs(JZTableWidget::item(row, col));

    // /Transfer/: 所有权交给本表格(基类用 unique_ptr 接管), 翻转后 JS 侧那份变成"非拥有",
    // GC 时不会再重复释放。
    item.setCppOwner(true);

    JZTableWidget::setItem(row, col, raw);

    // 用 JS 侧那份包装直接当开关(共享 PointInfo): 之后表格删除该项时, 失效能传回 JS。
    return adoptSwitch(raw, item);
}

jsbind::JSRawPointer<JZTableWidgetItem> JZJSTableWidget::setItemJs(int row, int col, const std::string& text)
{
    // 与 JZJSListWidget::appendItemJs(文本) 同思路: 基类不返回句柄, 这里自己 new 再交给基类接管。
    JZTableWidgetItem* it = new JZTableWidgetItem(text);

    invalidateItemJs(JZTableWidget::item(row, col));   // 顶掉旧格前先失效

    JZTableWidget::setItem(row, col, it);
    return switchFor(it);
}

void JZJSTableWidget::clearItemJs(int row, int col)
{
    if (!JZTableWidget::item(row, col))
        return;

    invalidateItemJs(JZTableWidget::item(row, col));   // 下面就会 delete, 先失效
    JZTableWidget::setItem(row, col, nullptr);
}

// ---------- 摘除 ----------

void JZJSTableWidget::removeItemJs(int row, int col)
{
    JZTableWidgetItem* it = JZTableWidget::item(row, col);
    if (!it)
        return;

    invalidateItemJs(it);                       // 下面就会 delete 该项, 先让 JS 侧包装失效
    delete JZTableWidget::takeItem(row, col);   // Qt 惯用法: 摘除并删除
}

jsbind::JSRawPointer<JZTableWidgetItem> JZJSTableWidget::takeItemJs(int row, int col)
{
    JZTableWidgetItem* it = JZTableWidget::item(row, col);
    if (!it)
        return jsbind::JSRawPointer<JZTableWidgetItem>();

    JZTableWidget::takeItem(row, col);          // 摘除, 所有权交回调用方

    // /TransferBack/: 交回 JS 拥有。复用同一个开关(共享 PointInfo), 因此 JS 侧原先持有的
    // 那份借用包装会一起变成"拥有者" —— 它们共享一个 PointInfo, 只会释放一次。
    jsbind::JSRawPointer<JZTableWidgetItem>& sw = switchFor(it);
    sw.setCppOwner(false);
    return sw;
}

void JZJSTableWidget::removeRowJs(int row)
{
    if (row < 0 || row >= rowCount())
        return;

    // 先失效该行所有格子对应的 JS 包装(下面 removeRow 会把它们删掉)
    for (int c = 0; c < columnCount(); c++)
        invalidateItemJs(JZTableWidget::item(row, c));

    JZTableWidget::removeRow(row);
    pruneItemJs();
}

void JZJSTableWidget::removeColumnJs(int col)
{
    if (col < 0 || col >= columnCount())
        return;

    for (int r = 0; r < rowCount(); r++)
        invalidateItemJs(JZTableWidget::item(r, col));

    JZTableWidget::removeColumn(col);
    pruneItemJs();
}

void JZJSTableWidget::clearContentsJs()
{
    invalidateAllItemJs();                      // 基类会 delete 全部 item, 先失效
    JZTableWidget::clearContents();
}

void JZJSTableWidget::clearJs()
{
    invalidateAllItemJs();
    JZTableWidget::clear();
}
