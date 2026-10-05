#include "BTERubberBand.h"

#include "JZColor.h"
#include "JZMouseEvent.h"
#include "JZPainter.h"
#include "JZPainterEvent.h"

JZ_OBJECT_IMPL(BTERubberBand, JZWidget)

// 原版 m_selectWidget 的样式: border: 2px solid rgb(255, 255, 127) —— 只描边、不填充。
static const int kBorderWidth = 2;
static JZColor rubberColor()
{
    return JZColor(255, 255, 127);
}

BTERubberBand::BTERubberBand(JZWidget* parent)
    : JZWidget(parent)
    , sigSelected(this)
{
    // 平时不显示; 0 = 透明覆盖层(不遮挡身后兄弟控件的重绘剔除)
    setVisible(false);
    setOpaque(0);
}

void BTERubberBand::begin(const JZPoint& originGlobal)
{
    m_originGlobal = originGlobal;
    m_origin = JZPoint(0, 0);
    m_current = JZPoint(0, 0);
    m_hasOrigin = false;
    update();
}

JZRect BTERubberBand::selection() const
{
    // 规范化: 拖拽可能朝四个方向, 统一成"左上 + 非负宽高"
    const int x1 = m_origin.x() < m_current.x() ? m_origin.x() : m_current.x();
    const int y1 = m_origin.y() < m_current.y() ? m_origin.y() : m_current.y();
    const int x2 = m_origin.x() > m_current.x() ? m_origin.x() : m_current.x();
    const int y2 = m_origin.y() > m_current.y() ? m_origin.y() : m_current.y();
    return JZRect(x1, y1, x2 - x1, y2 - y1);
}

void BTERubberBand::mouseMoveEvent(JZMouseEvent* event)
{
    // 起点校准: 卡片把"按下时的屏幕坐标"传了进来, 而这里拿到的是"移动时的屏幕 + 局部坐标"。
    // 两点都是屏幕坐标, 差值搬到局部坐标里等价(本控件无缩放/旋转) ⇒
    //     origin = localNow - (globalNow - originGlobal)
    // 这样不需要 global -> local 的反向换算(引擎只有 mapTo/mapToGlobal 正向)。
    if (!m_hasOrigin)
    {
        const JZPoint g = event->globalPos();
        m_origin = JZPoint(event->localPos().x() - (g.x() - m_originGlobal.x()),
                           event->localPos().y() - (g.y() - m_originGlobal.y()));
        m_hasOrigin = true;
    }

    m_current = event->localPos();
    update();
}

void BTERubberBand::mouseReleaseEvent(JZMouseEvent* event)
{
    m_current = event->localPos();
    if (!m_hasOrigin)
    {
        // 极快的拖拽: 抓取生效后直接松开, 一次 move 都没来。用当前点当起点 ⇒ 空框 ⇒
        // 不命中任何卡片(无害), 比瞎猜一个起点安全。
        m_origin = m_current;
        m_hasOrigin = true;
    }

    // 先收起自己再报事件: 调用方在槽里做命中判定 + 解除抓取, 期间不该再看到选择框
    hide();
    sigSelected.emit(event->modifiers());
}

void BTERubberBand::paintEvent(JZPainterEvent* event)
{
    (void)event;

    // 引擎没有 CSS 边框, 直接画 4 条 2px 边(不填充 —— 与原版"只有边框"一致)
    const JZRect r = selection();
    if (r.width() <= 0 || r.height() <= 0)
        return;

    JZPainter p(this);
    const JZColor c = rubberColor();

    p.fillRect(JZRect(r.x(), r.y(), r.width(), kBorderWidth), c);                      // 上
    p.fillRect(JZRect(r.x(), r.y() + r.height() - kBorderWidth, r.width(), kBorderWidth), c);   // 下
    p.fillRect(JZRect(r.x(), r.y(), kBorderWidth, r.height()), c);                     // 左
    p.fillRect(JZRect(r.x() + r.width() - kBorderWidth, r.y(), kBorderWidth, r.height()), c);   // 右
}
