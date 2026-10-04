// BTEItemWidget —— 通道卡片(自绘; 见头文件说明)。原版的 500 行自绘这里按"骨架够用"实现:
// 背景/边框/标题行/数值行/勾选标记/报警配色 + 左键勾选。
#include "BTEItemWidget.h"

#include "JZPainter.h"
#include "JZFontMetrics.h"
#include "JZMouseEvent.h"

#include <algorithm>

JZ_OBJECT_IMPL(BTEItemWidget, JZWidget)

// 原版的报警配色(BTEItemWidget.cpp)
static const JZColor kFaultBg(180, 40, 80);        // 故障未清除: 红
static const JZColor kFaultClearedBg(255, 140, 0); // 已确认: 橙
static const JZColor kNormalBg(0xff, 0xff, 0xff);
static const JZColor kCheckedBg(0xe8, 0xf2, 0xff);
static const JZColor kBorderIdle(0xC0, 0xC4, 0xCC);
static const JZColor kBorderChecked(0x2A, 0x7F, 0xFF);
static const JZColor kBorderHover(0x8a, 0x9b, 0xb5);
static const JZColor kText(0x20, 0x20, 0x20);
static const JZColor kTextOnFault(0xff, 0xff, 0xff);

BTEItemWidget::BTEItemWidget(JZWidget* parent)
    : JZWidget(parent)
    , sigClicked(this)
    , sigToggled(this)
{
    setScale(1);   // 原版构造尾: setSize(Item_Normal) 里会 setScale(m_scale) -> setFixedSize
}

// ---------------------------------------------------------------------------
// 尺寸: **固定尺寸**(照原版 setScale: setFixedSize(baseW*scale + (scale-1)*gap, baseH))。
// 于是布局压不动它 —— 8 列放不下时由外层 scrollArea 出滚动条, 而不是把卡片挤小。
// ---------------------------------------------------------------------------
JZSize BTEItemWidget::fixedSize() const
{
    const int gap = 3;                                  // 原版 setScale 里的 gap
    const int baseW = (m_size == Item_Small) ? 120 : 170;
    const int baseH = (m_size == Item_Small) ? 218 : 272;
    return JZSize(baseW * m_scale + (m_scale - 1) * gap, baseH);
}

JZSize BTEItemWidget::sizeHint() const
{
    return fixedSize();
}

JZSize BTEItemWidget::minimumSizeHint() const
{
    return fixedSize();   // 固定尺寸: 最小 == 期望 (原版 setFixedSize 就是这个语义)
}

// ---------------------------------------------------------------------------
// 数据/状态
// ---------------------------------------------------------------------------
void BTEItemWidget::setItemIndex(int idx)
{
    if (m_itemIndex == idx)
        return;
    m_itemIndex = idx;
    update();
}

void BTEItemWidget::setChannelText(const std::string& text)
{
    if (m_channelText == text)
        return;
    m_channelText = text;
    update();
}

void BTEItemWidget::setCheck(bool flag)
{
    if (m_checked == flag)
        return;
    m_checked = flag;
    update();
    sigToggled.emit(m_checked);   // 与 Qt 的 toggled(bool) 同口径: 只在真的变化时发
}

void BTEItemWidget::setAlarm(bool on)
{
    if (m_alarm == on)
        return;
    m_alarm = on;
    update();
}

void BTEItemWidget::setAlarmCleared(bool on)
{
    if (m_alarmCleared == on)
        return;
    m_alarmCleared = on;
    update();
}

void BTEItemWidget::setSize(ItemSize s)
{
    if (m_size == s)
    {
        setScale(m_scale);   // 原版 setSize 尾也会走一次 setScale(尺寸可能变)
        return;
    }
    m_size = s;
    setScale(m_scale);      // 原版: setSize 里按新 base 重新 setFixedSize
    update();
    updateGeometry();       // 尺寸提示变了: 让父布局重排
}

void BTEItemWidget::setScale(int scale)
{
    if (scale < 1)
        scale = 1;
    m_scale = scale;

    // 原版 setScale 的全部内容: 固定尺寸(宽度 = 占用列数 * 基准宽 + 间隔)
    const JZSize s = fixedSize();
    setFixedSize(s.width(), s.height());

    update();
    updateGeometry();
}

void BTEItemWidget::setValueLines(const std::vector<std::string>& lines)
{
    m_lines = lines;
    update();
}

void BTEItemWidget::clear()
{
    m_itemIndex = -1;
    m_channelText.clear();
    m_lines.clear();
    m_alarm = false;
    m_alarmCleared = false;
    setCheck(false);
    update();
}

std::string BTEItemWidget::getTitle() const
{
    // 原版: 标题 = 通道文本(没有就退化成序号)
    if (!m_channelText.empty())
        return m_channelText;
    return (m_itemIndex >= 0) ? ("#" + std::to_string(m_itemIndex + 1)) : std::string("#");
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------
void BTEItemWidget::paintEvent(JZPainterEvent*)
{
    JZPainter p(this);
    const int w = width();
    const int h = height();

    // ---- 底色 / 边框: 报警(红/橙) > 勾选(浅蓝) > 常态(白) ----
    JZColor bg = kNormalBg;
    JZColor fg = kText;
    if (m_alarm)
    {
        bg = m_alarmCleared ? kFaultClearedBg : kFaultBg;
        fg = kTextOnFault;
    }
    else if (m_checked)
    {
        bg = kCheckedBg;
    }

    JZColor border = kBorderIdle;
    if (m_checked)
        border = kBorderChecked;
    else if (m_hover)
        border = kBorderHover;

    p.setBrush(JZBrush(bg));
    p.setPen(JZPen(border, m_checked ? 2 : 1));
    p.drawRoundRect(0, 0, w, h, 4);

    // ---- 标题行: [x] 通道文本 …… 右端序号 ----
    const int headerH = (m_size == Item_Small) ? 18 : 22;
    JZFont titleFont((m_size == Item_Small) ? 11 : 13);
    titleFont.setBold(true);
    p.setFont(titleFont);
    p.setPen(JZPen(fg, 1));

    const std::string mark = m_checked ? "[x] " : "[ ] ";
    const int ty = JZFontMetrics::centerY(titleFont, headerH);
    p.drawText(6, ty, (mark + getTitle()).c_str());

    if (m_itemIndex >= 0)
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "#%d", m_itemIndex + 1);
        const int tw = JZFontMetrics::textWidth(buf, titleFont);
        p.drawText(w - 6 - tw, ty, buf);
    }

    // 标题下的分隔线
    p.setPen(JZPen(m_alarm ? JZColor(0xff, 0xff, 0xff) : kBorderIdle, 1));
    p.drawLine(4, headerH, w - 4, headerH);

    // ---- 数值行(最多画到装得下) ----
    JZFont lineFont((m_size == Item_Small) ? 10 : 12);
    p.setFont(lineFont);
    p.setPen(JZPen(fg, 1));

    const int lineH = (m_size == Item_Small) ? 13 : 17;
    int y = headerH + 4;
    for (size_t i = 0; i < m_lines.size(); ++i)
    {
        if (y + lineH > h - 2)
        {
            if (y + lineH > h)
                break;
        }
        p.drawText(6, y + JZFontMetrics::centerY(lineFont, lineH), m_lines[i].c_str());
        y += lineH;
        if (y > h)
            break;
    }
}

// ---------------------------------------------------------------------------
// 交互: 左键切换勾选(原版: 点击切换 + 发 toggled; Ctrl 多选在上层处理)
// ---------------------------------------------------------------------------
void BTEItemWidget::mousePressEvent(JZMouseEvent* event)
{
    if (event->button() != JZMouseEvent::LeftButton)
    {
        JZWidget::mousePressEvent(event);   // 右键走框架的上下文菜单流程
        return;
    }

    setFocus();
    if (!isEnabled())
        return;

    m_pressed = true;
    update();
}

void BTEItemWidget::mouseReleaseEvent(JZMouseEvent* event)
{
    if (event->button() == JZMouseEvent::LeftButton && m_pressed)
    {
        m_pressed = false;
        setCheck(!m_checked);      // 内部发 sigToggled
        sigClicked.emit();
        return;
    }
    JZWidget::mouseReleaseEvent(event);
}

void BTEItemWidget::enterEvent(JZEvent* event)
{
    m_hover = true;
    update();
    JZWidget::enterEvent(event);
}

void BTEItemWidget::leaveEvent(JZEvent* event)
{
    m_hover = false;
    update();
    JZWidget::leaveEvent(event);
}
