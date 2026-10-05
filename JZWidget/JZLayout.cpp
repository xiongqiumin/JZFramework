#include "JZLayout.h"
#include "JZWidget.h"
#include "JZLog.h"
#include <algorithm>
#include <climits>

JZ_OBJECT_IMPL(JZLayout, JZObject)

namespace {

// a 是不是 b 的祖先(沿 b 的父链往上找)。用来区分"真把控件放错了容器"与"容器是布局宿主
// 转发的内部子控件"这两种情况, 见下面 reparent 处的判定。
bool isAncestorOf(JZWidget* a, JZWidget* b)
{
    for (JZWidget* p = b; p != nullptr; p = p->parentWidget())
    {
        if (p == a)
            return true;
    }
    return false;
}

// 控件的父 p 与布局容器 container 的关系是否"正常"(不需要告警):
//   p == container                  —— 本来就挂在容器下(常态);
//   p 是 container 的祖先            —— 典型: GroupBox 的布局被转发到内部 contentWidget,
//                                      uic 把控件父设成了 GroupBox 本身(见 JZGroupBox::setLayout);
//   container 是 p 的祖先            —— 控件已在容器这棵子树里, 只是层级更深(提升到容器直接子级);
// 只有互不为祖先(控件挂在另一棵子树上)才是真误用, 那时才值得告警。
bool isReparentNormal(JZWidget* p, JZWidget* container)
{
    return !p || p == container || isAncestorOf(p, container) || isAncestorOf(container, p);
}

// 在单条轴上, 依据 min/max/weight 把可用空间分配给各单元。
// hint/min/max/weight 长度均为 n; weight>0 表示该项参与增长(也会参与收缩),
// weight==0 的项严格保持 sizeHint (不会被压, 也不会被拉伸)。
std::vector<int> distributeAxis(int avail,
                                const std::vector<int>& hint,
                                const std::vector<int>& mn,
                                const std::vector<int>& mx,
                                const std::vector<int>& weight)
{
    std::vector<int> size(hint.size(), 0);
    int usedFixed = 0;   // weight==0 项占用
    int usedFlex = 0;    // weight>0 项的初始 hint 占用 (增长时以此为基准)
    for (size_t i = 0; i < hint.size(); ++i) {
        size[i] = std::max(mn[i], std::min(hint[i], mx[i]));
        if (weight[i] > 0) usedFlex += size[i];
        else               usedFixed += size[i];
    }

    int delta = avail - usedFixed - usedFlex;   // >0 待增长, <0 待收缩
    const int n = (int)size.size();

    auto floor = [](long long num, long long den) -> int {  // 保证非负
        return (num <= 0) ? 0 : (int)(num / den);
    };

    // 把 delta 个像素按 weight 比例补/扣到各弹性项。逐轮逼近直到分完:
    // 每轮内先按比例整分, 余下的零星像素逐个消化, 因此不会超分配。
    auto distribute = [&](long long need, bool grow) -> void {
        if (need <= 0) return;
        for (int guard = 0; need > 0 && guard < 65536; ++guard) {
            int wsum = 0;
            for (int i = 0; i < n; ++i) {
                bool ok = grow ? (size[i] < mx[i]) : (size[i] > mn[i]);
                if (weight[i] > 0 && ok) wsum += weight[i];
            }
            if (wsum == 0) break;
            int gave = 0;
            for (int i = 0; i < n; ++i) {
                bool ok = grow ? (size[i] < mx[i]) : (size[i] > mn[i]);
                if (!(weight[i] > 0 && ok)) continue;
                long long g = floor(need * (long long)weight[i], wsum);
                long long room = grow ? (long long)mx[i] - size[i]
                                      : (long long)size[i] - mn[i];
                if (g > room) g = room;
                if (g > 0) {
                    size[i] += grow ? (int)g : -(int)g;
                    gave += (int)g;
                }
            }
            need -= gave;
            // 取整/上限截断后的零星余额: 逐像素在仍可变化的弹性项间消化
            for (int i = 0; need > 0 && i < n; ++i) {
                if (weight[i] <= 0) continue;
                if (grow ? (size[i] < mx[i]) : (size[i] > mn[i])) {
                    size[i] += grow ? 1 : -1;
                    --need;
                }
            }
        }
    };

    if (delta > 0)
        distribute((long long)delta, true);
    else if (delta < 0)
        distribute(-(long long)delta, false);

    // 硬上限: **容器是硬边界**。上面那轮收缩只动"弹性项且 size>min"的项 —— 固定项、以及
    // 已经贴到最小尺寸的项, 会把合计顶出容器(表现就是子控件画到父控件外面/盖住邻栏)。
    // 这里兜底: 合计超出 avail 就按当前尺寸等比削到 avail(削到 0 也认), 保证加起来不超。
    long long total = 0;
    for (int i = 0; i < n; ++i)
        total += size[i];
    if (total > avail && total > 0) {
        int used = 0;
        for (int i = 0; i < n; ++i) {
            size[i] = (int)((long long)size[i] * avail / total);
            used += size[i];
        }
        // 等比取整剩下的零星像素: 逐个补回(rest < n), 让合计恰好落在 avail 内
        int rest = avail - used;
        for (int i = 0; rest > 0 && i < n; ++i) {
            ++size[i];
            --rest;
        }
    }
    return size;
}

// 倾向占据更多空间的策略 (参考 Qt 的 Expanding/MinimumExpanding/Ignored)
inline bool policyExpands(JZWidget::SizePolicy p)
{
    return p == JZWidget::SizePolicy::Expanding
        || p == JZWidget::SizePolicy::MinimumExpanding
        || p == JZWidget::SizePolicy::Ignored;
}

} // namespace

// ==================== JZLayout ====================

JZLayout::~JZLayout()
{
    // 布局销毁时解除与所有受管控件的关联。m_managed 用 JZObjectPointer 跟踪,
    // 控件已销毁的项 object() 返回 nullptr, 这里跳过避免访问悬垂指针。
    for (auto& ptr : m_managed)
        if (JZWidget* w = static_cast<JZWidget*>(ptr.object()))
            w->_layoutDetach(this);
    m_managed.clear();

    // 布局没了: 通知宿主控件清掉它的布局指针 —— Qt 的 `delete w->layout()` 正是靠这个行为
    // 让 widget->layout() 立刻变空, 之后才能再 setLayout 新布局(见 JZWidget::setLayout 的 Qt 语义)。
    if (m_container)
        m_container->_clearLayoutPointer(this);
}

void JZLayout::addManaged(JZWidget* widget)
{
    if (!widget)
        return;
    for (auto& ptr : m_managed)
        if (ptr.object() == widget)
            return;   // 已登记
    // 布局已 apply 到容器(setLayout 之后)时, 新增控件必须立刻挂到容器 child 树,
    // 否则渲染遍历(w->children())看不到它, 整窗白屏 —— 这正是 test_jswidget 白屏的根因
    // (main.ts 先 setLayout 再 addWidget, 而 apply() 只在 setLayout 那一刻 reparent)。
    // 仅当 m_container 为空(先 addWidget 再 setLayout)时交给 apply() 统一 reparent。
    // 与 apply() 同一条规则: 已属其它父控件则强制 reparent 到本容器(告警便于定位误用)。
    if (m_container)
    {
        JZWidget* p = widget->parentWidget();
        if (!isReparentNormal(p, m_container))
        {
            LOG_W("JZLayout::addWidget: 控件\"" + widget->objectName() + "\"已被父控件\"" +
                  p->objectName() + "\"持有, 但被加入布局容器\"" +
                  m_container->objectName() + "\", 将被强制 reparent 到本容器");
        }
        if (p != m_container)
            widget->setParentWidget(m_container);
    }
    m_managed.push_back(widget->guardPointer());
    widget->_layoutAttach(this);
}

void JZLayout::removeManaged(JZWidget* widget)
{
    if (!widget)
        return;
    for (auto it = m_managed.begin(); it != m_managed.end(); ++it)
    {
        if (it->object() == widget)
        {
            m_managed.erase(it);
            break;
        }
    }
    widget->_layoutDetach(this);
}

void JZLayout::apply(JZWidget* container)
{
    // 布局换宿主(Qt 里 setLayout 会把属于别的控件的布局 reparent 过来): 旧宿主不能继续指向本布局,
    // 否则两边析构都会 delete 它(双重释放)。
    if (m_container && m_container != container)
        m_container->_clearLayoutPointer(this);

    m_container = container;
    // 支持"先 addWidget 再 setLayout"的写法: apply 时把已登记的管理控件
    // 挂到容器下 (等价 Qt QLayout::addWidget 的自动 reparent), 这样无需在
    // 加控件前逐个 setParent。容器即布局的父是唯一合法归属, 与框架约束一致。
    for (auto& ptr : m_managed)
    {
        JZWidget* w = static_cast<JZWidget*>(ptr.object());
        if (!w)
            continue;
        JZWidget* p = w->parentWidget();
        // addWidget 的控件若被**无关**的父控件持有(互不为祖先), 属于误用: 布局会强制 reparent
        // 到本容器, 此处给出告警便于定位。父子互为祖先不算误用(见 isReparentNormal 的说明)。
        if (!isReparentNormal(p, container))
        {
            LOG_W("JZLayout::apply: 控件\"" + w->objectName() + "\"已被父控件\"" +
                  p->objectName() + "\"持有, 但被加入布局容器\"" +
                  container->objectName() + "\", 将被强制 reparent 到本容器");
        }
        if (p != container)
            w->setParentWidget(container);
    }
    relayout();
}

void JZLayout::relayout()
{
    purgeInvalid();   // 先剔除已销毁却未 remove 的受管控件, 防止后续访问悬垂
    if (m_container) {
        m_container->updateLayout();   // 重排自身子控件(含本次变更)
        m_container->updateGeometry(); // 向上级联: 父布局按新 sizeHint 重排
    }
}

void JZLayout::purgeInvalid()
{
    // 兜底: 控件销毁(直接 delete 或经 removeWidget)后, JZObjectPointer 失效,
    // 这里静默剔除已销毁的受管控件, 避免 m_managed 持有悬垂指针。直接 delete
    // 控件是合法写法, 无需告警。
    for (auto it = m_managed.begin(); it != m_managed.end(); )
    {
        if (!*it)   // JZObjectPointer 失效 == 控件已销毁
            it = m_managed.erase(it);
        else
            ++it;
    }
}

bool JZLayout::isManagedLive(JZWidget* w) const
{
    if (!w)
        return false;
    for (const auto& ptr : m_managed)
        if (ptr.object() == w)
            return true;
    return false;
}

// ==================== JZBoxLayout ====================

JZBoxLayout::JZBoxLayout(int pad, int spacing)
{
    if (pad > 0)
        setContentsMargins(pad, pad, pad, pad);
    m_gap = spacing;
}

JZBoxLayout::~JZBoxLayout()
{
    // 受管登记的解除由基类 ~JZLayout 统一完成
}

void JZBoxLayout::addWidget(JZWidget* widget, int stretch)
{
    Item it;
    it.type = WidgetItem;
    it.widget = widget;
    it.stretch = stretch < 0 ? 0 : stretch;
    m_items.push_back(it);
    if (widget)
        addManaged(widget);
    relayout();
}

void JZBoxLayout::addSpacing(int size)
{
    Item it;
    it.type = SpacerItem;
    it.spaceSize = size < 0 ? 0 : size;
    m_items.push_back(it);
    relayout();
}

void JZBoxLayout::addStretch(int stretch)
{
    Item it;
    it.type = StretchItem;
    it.stretch = stretch <= 0 ? 1 : stretch;
    m_items.push_back(it);
    relayout();
}

void JZBoxLayout::insertWidget(int index, JZWidget* widget, int stretch)
{
    Item it;
    it.type = WidgetItem;
    it.widget = widget;
    it.stretch = stretch < 0 ? 0 : stretch;
    if (index < 0 || index >= (int)m_items.size())
        m_items.push_back(it);
    else
        m_items.insert(m_items.begin() + index, it);
    if (widget)
        addManaged(widget);
    relayout();
}

void JZBoxLayout::insertSpacing(int index, int size)
{
    Item it;
    it.type = SpacerItem;
    it.spaceSize = size < 0 ? 0 : size;
    if (index < 0 || index >= (int)m_items.size())
        m_items.push_back(it);
    else
        m_items.insert(m_items.begin() + index, it);
    relayout();
}

void JZBoxLayout::insertStretch(int index, int stretch)
{
    Item it;
    it.type = StretchItem;
    it.stretch = stretch <= 0 ? 1 : stretch;
    if (index < 0 || index >= (int)m_items.size())
        m_items.push_back(it);
    else
        m_items.insert(m_items.begin() + index, it);
    relayout();
}

void JZBoxLayout::removeAt(int index)
{
    if (index < 0 || index >= (int)m_items.size())
        return;
    if (m_items[index].type == WidgetItem && m_items[index].widget)
        removeManaged(m_items[index].widget);
    m_items.erase(m_items.begin() + index);
    relayout();
}

void JZBoxLayout::clear()
{
    if (m_items.empty())
        return;
    for (const auto& it : m_items)
    {
        if (it.type == WidgetItem && it.widget && isManagedLive(it.widget))
            removeManaged(it.widget);
    }
    m_items.clear();
    relayout();
}

void JZBoxLayout::removeWidget(JZWidget* widget)
{
    if (!widget)
        return;
    for (int i = 0; i < (int)m_items.size(); ++i)
    {
        if (m_items[i].type == WidgetItem && m_items[i].widget == widget)
        {
            removeAt(i);
            return;
        }
    }
}

void JZBoxLayout::updateLayout()
{
    if (!m_container)
        return;

    int cw = m_container->width();
    int ch = m_container->height();
    int innerW = std::max(0, cw - m_marginLeft - m_marginRight);
    int innerH = std::max(0, ch - m_marginTop - m_marginBottom);
    bool horiz = (flexFlow() == 0);

    int n = (int)m_items.size();
    if (n == 0)
        return;

    // 隐藏控件不参与布局 (等价 Qt: 隐藏项被布局剔除) —— 不占主轴/交叉轴空间、
    // 不定位、也不贡献 item 间间隔。先统计"参与"的 item 数, 用于计算间隔总数。
    int participating = 0;
    for (int i = 0; i < n; ++i) {
        const auto& it = m_items[i];
        if (it.type == WidgetItem) {
            if (it.widget && it.widget->isVisible())
                ++participating;
        } else {
            ++participating;   // Spacer / Stretch 始终参与
        }
    }

    int gapTotal = participating > 0 ? m_gap * (participating - 1) : 0;
    int mainAvail = std::max(0, (horiz ? innerW : innerH) - gapTotal);
    int crossAvail = (horiz ? innerH : innerW);
    int crossOrigin = (horiz ? m_marginTop : m_marginLeft);

    // 主轴上每个 item 的 hint/min/max/weight; 隐藏控件全置 0 (不占空间、无权重)。
    std::vector<int> mainHint(n, 0), mainMin(n, 0), mainMax(n, INT_MAX), mainWeight(n, 0);
    for (int i = 0; i < n; ++i) {
        const auto& it = m_items[i];
        if (it.type == WidgetItem && it.widget && isManagedLive(it.widget)) {
            if (!it.widget->isVisible())
                continue;   // 隐藏: 不占空间、无权重
            JZSize hs = it.widget->sizeHint();
            JZWidget::SizePolicy mp = horiz ? it.widget->horizontalPolicy() : it.widget->verticalPolicy();
            mainHint[i] = horiz ? hs.width() : hs.height();
            mainMin[i]  = horiz ? it.widget->minimumWidth()  : it.widget->minimumHeight();
            mainMax[i]  = horiz ? it.widget->maximumWidth()  : it.widget->maximumHeight();
            if (it.stretch > 0)      mainWeight[i] = it.stretch;
            else if (policyExpands(mp)) mainWeight[i] = 1;
            else                     mainWeight[i] = 0;
        } else if (it.type == SpacerItem) {
            int s = std::max(0, it.spaceSize);
            mainHint[i] = s; mainMin[i] = s; mainMax[i] = s; mainWeight[i] = 0;
        } else { // StretchItem
            mainHint[i] = 0; mainMin[i] = 0; mainMax[i] = INT_MAX;
            mainWeight[i] = it.stretch > 0 ? it.stretch : 1;
        }
    }

    std::vector<int> mainSize = distributeAxis(mainAvail, mainHint, mainMin, mainMax, mainWeight);

    // 沿主轴依次摆放, 交叉轴按策略居中/填满; 隐藏控件直接跳过(不定位、不前进)。
    int cursor = horiz ? m_marginLeft : m_marginTop;
    for (int i = 0; i < n; ++i) {
        const auto& it = m_items[i];
        if (it.type == WidgetItem && it.widget && isManagedLive(it.widget)) {
            if (!it.widget->isVisible())
                continue;   // 隐藏: 不参与摆放
            JZSize hs = it.widget->sizeHint();
            JZWidget::SizePolicy cp = horiz ? it.widget->verticalPolicy() : it.widget->horizontalPolicy();
            int hintCross = horiz ? hs.height() : hs.width();
            int cmn = horiz ? it.widget->minimumHeight() : it.widget->minimumWidth();
            int cmx = horiz ? it.widget->maximumHeight() : it.widget->maximumWidth();
            // 交叉轴: 除 Fixed 外一律填满 (HBox 子项填满高度 / VBox 子项填满宽度)
            bool fillCross = it.stretch > 0 || cp != JZWidget::SizePolicy::Fixed;
            int desired = fillCross ? crossAvail : hintCross;
            // 先按 hint/max 求期望尺寸, 再**用容器可用空间兜底** —— 子控件的最小尺寸比容器还大时
            // 取容器可用空间(而不是 min), 否则它会顶出容器两端(居中摆放时左边还会盖住邻栏)。
            int crossSize = std::max(0, std::min(crossAvail,
                                    std::max(cmn, std::min(desired, cmx))));
            int crossPos = crossOrigin + (crossAvail - crossSize) / 2;   // crossSize<=crossAvail ⇒ 不会越界
            int x = horiz ? cursor     : crossPos;
            int y = horiz ? crossPos   : cursor;
            int w = horiz ? mainSize[i] : crossSize;
            int h = horiz ? crossSize   : mainSize[i];
            it.widget->setGeometry(x, y, w, h);
        }
        cursor += mainSize[i] + m_gap;
    }
}

JZSize JZBoxLayout::sizeHint() const
{
    int n = (int)m_items.size();
    if (n == 0 || !m_container)
        return JZSize(m_marginLeft + m_marginRight, m_marginTop + m_marginBottom);

    bool horiz = (flexFlow() == 0);
    int main = 0;
    int cross = 0;
    int participating = 0;   // 隐藏控件不计入 hint 与间隔 (与 updateLayout 一致)
    for (int i = 0; i < n; ++i) {
        const auto& it = m_items[i];
        if (it.type == WidgetItem && it.widget && isManagedLive(it.widget)) {
            if (!it.widget->isVisible())
                continue;   // 隐藏: 不计 hint
            JZSize hs = it.widget->sizeHint();
            if (horiz) { main += hs.width();  cross = std::max(cross, hs.height()); }
            else       { main += hs.height(); cross = std::max(cross, hs.width()); }
            ++participating;
        } else if (it.type == SpacerItem) {
            main += std::max(0, it.spaceSize);
            ++participating;
        }
        // StretchItem 无固有尺寸, 不计入 hint, 但参与 item 间间隔
        else {
            ++participating;
        }
    }
    main += m_gap * (participating > 0 ? participating - 1 : 0);
    int w = horiz ? (m_marginLeft + m_marginRight + main)
                  : (m_marginLeft + m_marginRight + cross);
    int h = horiz ? (m_marginTop + m_marginBottom + cross)
                  : (m_marginTop + m_marginBottom + main);
    return JZSize(w, h);
}

JZSize JZBoxLayout::minimumSizeHint() const
{
    // 与 sizeHint 同结构, 但取子控件 minimumWidth/Height (而非 sizeHint),
    // 作为容器可缩放的下限, 防止窗口/父布局把内容压到不可用。
    int n = (int)m_items.size();
    if (n == 0 || !m_container)
        return JZSize(m_marginLeft + m_marginRight, m_marginTop + m_marginBottom);

    bool horiz = (flexFlow() == 0);
    int main = 0;
    int cross = 0;
    int participating = 0;
    for (int i = 0; i < n; ++i) {
        const auto& it = m_items[i];
        if (it.type == WidgetItem && it.widget && isManagedLive(it.widget)) {
            if (!it.widget->isVisible())
                continue;
            int mw = it.widget->minimumWidth();
            int mh = it.widget->minimumHeight();
            if (horiz) { main += mw;  cross = std::max(cross, mh); }
            else       { main += mh;  cross = std::max(cross, mw); }
            ++participating;
        } else if (it.type == SpacerItem) {
            main += std::max(0, it.spaceSize);
            ++participating;
        } else {
            ++participating;
        }
    }
    main += m_gap * (participating > 0 ? participating - 1 : 0);
    int w = horiz ? (m_marginLeft + m_marginRight + main)
                  : (m_marginLeft + m_marginRight + cross);
    int h = horiz ? (m_marginTop + m_marginBottom + cross)
                  : (m_marginTop + m_marginBottom + main);
    return JZSize(w, h);
}

// ==================== JZGridLayout ====================

JZGridLayout::JZGridLayout(int pad, int gap)
{
    if (pad > 0)
        setContentsMargins(pad, pad, pad, pad);
    m_gap = gap;
}

void JZGridLayout::addWidget(JZWidget* widget, int row, int col)
{
    addWidget(widget, row, col, 1, 1);
}

void JZGridLayout::addWidget(JZWidget* widget, int fromRow, int fromCol, int rowSpan, int colSpan)
{
    Item it;
    it.widget = widget;
    it.row = fromRow;
    it.col = fromCol;
    it.rowSpan = rowSpan > 0 ? rowSpan : 1;
    it.colSpan = colSpan > 0 ? colSpan : 1;
    m_items.push_back(it);
    if (widget)
        addManaged(widget);
    relayout();
}

void JZGridLayout::removeWidget(JZWidget* widget)
{
    if (!widget)
        return;
    for (size_t i = 0; i < m_items.size(); ++i)
    {
        if (m_items[i].widget == widget)
        {
            m_items.erase(m_items.begin() + i);
            removeManaged(widget);
            relayout();
            return;
        }
    }
}

void JZGridLayout::clear()
{
    if (m_items.empty())
        return;
    for (const auto& it : m_items)
    {
        if (it.widget)
            removeManaged(it.widget);
    }
    m_items.clear();
    relayout();
}

void JZGridLayout::setRowStretch(int row, int stretch)
{
    if (row < 0) return;
    if ((size_t)row >= m_rowSpecs.size()) m_rowSpecs.resize(row + 1);
    m_rowSpecs[row].stretch = stretch;
    relayout();
}

void JZGridLayout::setColumnStretch(int col, int stretch)
{
    if (col < 0) return;
    if ((size_t)col >= m_colSpecs.size()) m_colSpecs.resize(col + 1);
    m_colSpecs[col].stretch = stretch;
    relayout();
}

void JZGridLayout::setRowMinimumHeight(int row, int minHeight)
{
    if (row < 0) return;
    if ((size_t)row >= m_rowSpecs.size()) m_rowSpecs.resize(row + 1);
    m_rowSpecs[row].minSize = minHeight;
    relayout();
}

void JZGridLayout::setColumnMinimumWidth(int col, int minWidth)
{
    if (col < 0) return;
    if ((size_t)col >= m_colSpecs.size()) m_colSpecs.resize(col + 1);
    m_colSpecs[col].minSize = minWidth;
    relayout();
}

void JZGridLayout::setHorizontalSpacing(int spacing)
{
    m_hGap = spacing;
    relayout();
}

void JZGridLayout::setVerticalSpacing(int spacing)
{
    m_vGap = spacing;
    relayout();
}

int JZGridLayout::rowCount() const
{
    int maxR = (int)m_rowSpecs.size();
    for (const auto& it : m_items) {
        int end = it.row + it.rowSpan;
        if (end > maxR) maxR = end;
    }
    return maxR;
}

int JZGridLayout::columnCount() const
{
    int maxC = (int)m_colSpecs.size();
    for (const auto& it : m_items) {
        int end = it.col + it.colSpan;
        if (end > maxC) maxC = end;
    }
    return maxC;
}

void JZGridLayout::updateLayout()
{
    if (!m_container)
        return;

    int cw = m_container->width();
    int ch = m_container->height();
    int innerW = std::max(0, cw - m_marginLeft - m_marginRight);
    int innerH = std::max(0, ch - m_marginTop - m_marginBottom);
    int cols = columnCount();
    int rows = rowCount();
    if (cols <= 0 || rows <= 0)
        return;

    int hGap = horizontalSpacing();
    int vGap = verticalSpacing();

    std::vector<int> colHint(cols, 0), colMin(cols, 0), colMax(cols, INT_MAX), colW(cols, 0);
    std::vector<int> rowHint(rows, 0), rowMin(rows, 0), rowMax(rows, INT_MAX), rowW(rows, 0);
    std::vector<int> colWeight(cols, 0), rowWeight(rows, 0);

    // 每个单元格的 hint/min/max, 以及来自控件扩展策略的权重。
    // span>1 的控件把需求均摊到覆盖的各行列 (余数给首行/列), 避免独占某
    // 行/列的跨格控件被忽略导致该行列宽/高为 0 而被压扁。
    auto spreadMax = [](int value, int from, int span, std::vector<int>& arr) {
        if (span <= 1) { arr[from] = std::min(arr[from], value); return; }
        int base = value / span;
        int rem  = value % span;
        for (int c = from; c < from + span; ++c)
            arr[c] = std::min(arr[c], base + (c == from ? rem : 0));
    };
    auto spreadMin = [](int value, int from, int span, std::vector<int>& arr) {
        if (span <= 1) { arr[from] = std::max(arr[from], value); return; }
        int base = value / span;
        int rem  = value % span;
        for (int c = from; c < from + span; ++c)
            arr[c] = std::max(arr[c], base + (c == from ? rem : 0));
    };
    for (const auto& it : m_items) {
        if (!it.widget || !it.widget->isVisible() || !isManagedLive(it.widget)) continue;   // 隐藏/已销毁控件不参与网格布局
        JZSize hs = it.widget->sizeHint();
        spreadMin(hs.width(),  it.col, it.colSpan, colHint);
        spreadMin(hs.height(), it.row, it.rowSpan, rowHint);
        spreadMin(it.widget->minimumWidth(),  it.col, it.colSpan, colMin);
        spreadMin(it.widget->minimumHeight(), it.row, it.rowSpan, rowMin);
        if (it.widget->maximumWidth() < INT_MAX)
            spreadMax(it.widget->maximumWidth(), it.col, it.colSpan, colMax);
        if (it.widget->maximumHeight() < INT_MAX)
            spreadMax(it.widget->maximumHeight(), it.row, it.rowSpan, rowMax);
        if (policyExpands(it.widget->horizontalPolicy())) {
            for (int c = it.col; c < it.col + it.colSpan; ++c)
                colWeight[c] = std::max(colWeight[c], 1);
        }
        if (policyExpands(it.widget->verticalPolicy())) {
            for (int r = it.row; r < it.row + it.rowSpan; ++r)
                rowWeight[r] = std::max(rowWeight[r], 1);
        }
    }

    // 行列 stretch / 最小尺寸规格
    for (int c = 0; c < cols; ++c) {
        int s = (c < (int)m_colSpecs.size()) ? m_colSpecs[c].stretch : 0;
        if (s > 0) colWeight[c] = std::max(colWeight[c], s);
        if (c < (int)m_colSpecs.size() && m_colSpecs[c].minSize >= 0)
            colMin[c] = std::max(colMin[c], m_colSpecs[c].minSize);
    }
    for (int r = 0; r < rows; ++r) {
        int s = (r < (int)m_rowSpecs.size()) ? m_rowSpecs[r].stretch : 0;
        if (s > 0) rowWeight[r] = std::max(rowWeight[r], s);
        if (r < (int)m_rowSpecs.size() && m_rowSpecs[r].minSize >= 0)
            rowMin[r] = std::max(rowMin[r], m_rowSpecs[r].minSize);
    }

    colW = distributeAxis(innerW - hGap * (cols - 1), colHint, colMin, colMax, colWeight);
    rowW = distributeAxis(innerH - vGap * (rows - 1), rowHint, rowMin, rowMax, rowWeight);

    // 累计行列起点
    std::vector<int> colX(cols, 0), rowY(rows, 0);
    int cx = m_marginLeft;
    for (int c = 0; c < cols; ++c) { colX[c] = cx; cx += colW[c] + hGap; }
    int ry = m_marginTop;
    for (int r = 0; r < rows; ++r) { rowY[r] = ry; ry += rowW[r] + vGap; }

    // 放置每个控件 (处理合并单元格), 单元格内居中; 隐藏控件跳过不定位
    for (const auto& it : m_items) {
        if (!it.widget || !it.widget->isVisible() || !isManagedLive(it.widget)) continue;
        int cellX = colX[it.col];
        int cellY = rowY[it.row];
        int cellW = 0, cellH = 0;
        for (int c = it.col; c < it.col + it.colSpan; ++c) cellW += colW[c];
        for (int r = it.row; r < it.row + it.rowSpan; ++r) cellH += rowW[r];
        if (it.colSpan > 1) cellW += hGap * (it.colSpan - 1);
        if (it.rowSpan > 1) cellH += vGap * (it.rowSpan - 1);

        JZWidget::SizePolicy hp = it.widget->horizontalPolicy();
        JZWidget::SizePolicy vp = it.widget->verticalPolicy();
        bool hFill = policyExpands(hp) || hp == JZWidget::SizePolicy::Preferred;
        bool vFill = policyExpands(vp) || vp == JZWidget::SizePolicy::Preferred;

        int w = hFill ? cellW : it.widget->sizeHint().width();
        int h = vFill ? cellH : it.widget->sizeHint().height();
        w = std::max(it.widget->minimumWidth(),  std::min(w, it.widget->maximumWidth()));
        h = std::max(it.widget->minimumHeight(), std::min(h, it.widget->maximumHeight()));
        // **单元格是硬边界**: 控件的最小尺寸再大也不能越过自己的格子(否则会画到容器外)
        w = std::min(w, cellW);
        h = std::min(h, cellH);
        int gx = cellX + (cellW - w) / 2;
        int gy = cellY + (cellH - h) / 2;
        it.widget->setGeometry(gx, gy, w, h);
    }
}

JZSize JZGridLayout::sizeHint() const
{
    int cols = columnCount();
    int rows = rowCount();
    if (cols <= 0 || rows <= 0)
        return JZSize(m_marginLeft + m_marginRight, m_marginTop + m_marginBottom);

    std::vector<int> colHint(cols, 0), rowHint(rows, 0);
    for (const auto& it : m_items) {
        if (!it.widget || !it.widget->isVisible() || !isManagedLive(it.widget)) continue;   // 隐藏/已销毁控件不参与网格布局
        JZSize hs = it.widget->sizeHint();
        // 与 updateLayout 一致: 跨格控件按覆盖列数均摊, 余数给首列
        if (it.colSpan <= 1) {
            colHint[it.col] = std::max(colHint[it.col], hs.width());
        } else {
            int base = hs.width() / it.colSpan;
            int rem  = hs.width() % it.colSpan;
            for (int c = it.col; c < it.col + it.colSpan; ++c)
                colHint[c] = std::max(colHint[c], base + (c == it.col ? rem : 0));
        }
        if (it.rowSpan <= 1) {
            rowHint[it.row] = std::max(rowHint[it.row], hs.height());
        } else {
            int base = hs.height() / it.rowSpan;
            int rem  = hs.height() % it.rowSpan;
            for (int r = it.row; r < it.row + it.rowSpan; ++r)
                rowHint[r] = std::max(rowHint[r], base + (r == it.row ? rem : 0));
        }
    }
    int w = m_marginLeft + m_marginRight;
    int h = m_marginTop + m_marginBottom;
    for (int c = 0; c < cols; ++c) w += colHint[c];
    for (int r = 0; r < rows; ++r) h += rowHint[r];
    int hGap = horizontalSpacing();
    int vGap = verticalSpacing();
    w += hGap * (cols - 1);
    h += vGap * (rows - 1);
    return JZSize(w, h);
}

JZSize JZGridLayout::minimumSizeHint() const
{
    // 同 sizeHint 结构, 但取子控件 minimumWidth/Height 作为列/行最小尺寸。
    int cols = columnCount();
    int rows = rowCount();
    if (cols <= 0 || rows <= 0)
        return JZSize(m_marginLeft + m_marginRight, m_marginTop + m_marginBottom);

    std::vector<int> colMin(cols, 0), rowMin(rows, 0);
    for (const auto& it : m_items) {
        if (!it.widget || !it.widget->isVisible() || !isManagedLive(it.widget)) continue;
        int mw = it.widget->minimumWidth();
        int mh = it.widget->minimumHeight();
        if (it.colSpan <= 1) {
            colMin[it.col] = std::max(colMin[it.col], mw);
        } else {
            int base = mw / it.colSpan;
            int rem  = mw % it.colSpan;
            for (int c = it.col; c < it.col + it.colSpan; ++c)
                colMin[c] = std::max(colMin[c], base + (c == it.col ? rem : 0));
        }
        if (it.rowSpan <= 1) {
            rowMin[it.row] = std::max(rowMin[it.row], mh);
        } else {
            int base = mh / it.rowSpan;
            int rem  = mh % it.rowSpan;
            for (int r = it.row; r < it.row + it.rowSpan; ++r)
                rowMin[r] = std::max(rowMin[r], base + (r == it.row ? rem : 0));
        }
    }
    int w = m_marginLeft + m_marginRight;
    int h = m_marginTop + m_marginBottom;
    for (int c = 0; c < cols; ++c) w += colMin[c];
    for (int r = 0; r < rows; ++r) h += rowMin[r];
    int hGap = horizontalSpacing();
    int vGap = verticalSpacing();
    w += hGap * (cols - 1);
    h += vGap * (rows - 1);
    return JZSize(w, h);
}

// ==================== JZSelfLayout ====================

JZSelfLayout::JZSelfLayout(Anchor anchor, int dx, int dy)
    : m_anchor(anchor)
    , m_dx(dx)
    , m_dy(dy)
{
}

JZSelfLayout::~JZSelfLayout()
{
}

void JZSelfLayout::setAnchor(Anchor anchor, int dx, int dy)
{
    m_anchor = anchor;
    m_dx = dx;
    m_dy = dy;
    update();
}

void JZSelfLayout::update()
{
    if (!m_widget)
        return;
    JZWidget* parent = m_widget->parentWidget();
    if (!parent)
        return;

    int pw = parent->width();
    int ph = parent->height();
    int w = m_widget->width();
    int h = m_widget->height();

    int x = 0, y = 0;
    switch (m_anchor)
    {
    case TopLeft:      x = m_dx;                 y = m_dy;                 break;
    case TopCenter:    x = (pw - w) / 2 + m_dx;  y = m_dy;                 break;
    case TopRight:     x = pw - w - m_dx;        y = m_dy;                 break;
    case MiddleLeft:   x = m_dx;                 y = (ph - h) / 2 + m_dy; break;
    case Center:       x = (pw - w) / 2 + m_dx;  y = (ph - h) / 2 + m_dy; break;
    case MiddleRight:  x = pw - w - m_dx;        y = (ph - h) / 2 + m_dy; break;
    case BottomLeft:   x = m_dx;                 y = ph - h - m_dy;        break;
    case BottomCenter: x = (pw - w) / 2 + m_dx;  y = ph - h - m_dy;        break;
    case BottomRight:  x = pw - w - m_dx;        y = ph - h - m_dy;        break;
    }
    m_widget->move(x, y);
}