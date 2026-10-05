// BTEItemWidget —— 通道卡片(自绘)。绘制/配色/文案全部照原版 BTEServer/BTEItemWidget.cpp:
//   paintEvent / bgColor / getTitle / getDispInfo / setSize / setScale / clear / mouseReleaseEvent
// 只有 Qt -> 引擎的 API 替换(见头文件说明), 几何与颜色取值与原版逐一对齐。
#include "BTEItemWidget.h"

#include "Common.h"        // DeviceStatus
#include "DataTrans.h"     // fromVoltageUnit / fromElectricUnit / fromTempUnit / fromCapUnit / fromEngUnit / fromTimeUnitSec
#include "JZFontMetrics.h"
#include "JZKeyEvent.h"    // CtrlModifier(Ctrl + 左键多选)
#include "JZMouseEvent.h"
#include "JZPainter.h"

#include <algorithm>
#include <cstdio>

JZ_OBJECT_IMPL(BTEItemWidget, JZWidget)

// 故障卡片配色: 未清除为红色, 已清除(报警确认)后为橙色(原版)
static const JZColor kFaultBgColor(180, 40, 80);
static const JZColor kFaultClearedBgColor(255, 140, 0);

namespace {

// 4 位 / 1 位小数(QString::number(v, 'f', 4) 的等价物)
std::string num4(double v) { char b[32]; snprintf(b, sizeof(b), "%.4f", v); return b; }
std::string num1(double v) { char b[32]; snprintf(b, sizeof(b), "%.1f", v); return b; }

} // namespace

BTEItemWidget::BTEItemWidget(JZWidget* parent)
    : JZWidget(parent)
    , sigClicked(this)
    , sigToggled(this)
{
    setContextMenuPolicy(JZWidget::CustomContextMenu);   // 原版: setContextMenuPolicy(Qt::CustomContextMenu)

    m_labels = { "状态", "工步序号", "循环序号", "工步时间", "电压(V)",
                 "电流(A)", "温度(℃)", "充电容量(Ah)", "充电能量(Wh)" };

    m_scale = 1;
    m_fontTitle.setSize(14);     // 原版: m_fontTitle.setPixelSize(14)

    setSize(Item_Normal);        // 原版构造尾
    updateToolTip();
}

// ---------------------------------------------------------------------------
// 尺寸: 照原版 setSize/setScale —— 固定尺寸, 与窗口宽度无关(8 列放不下时由外层滚动区出滚动条)
//   Item_Normal: baseWidth 170 × baseHeight 272, 字体 12px
//   Item_Small : baseWidth 120 × baseHeight 218, 字体 9px
//   setFixedSize(baseWidth*scale + (scale-1)*gap, baseHeight), gap = 3
// ---------------------------------------------------------------------------
void BTEItemWidget::setSize(int itemSize)
{
    m_itemSize = itemSize;

    if (itemSize == Item_Normal)
    {
        m_font.setSize(12);
        m_baseWidth = 170;
        m_baseHeight = 272;   // 原版注释: "增加温度显示"
    }
    else
    {
        m_font.setSize(9);
        m_baseWidth = 120;
        m_baseHeight = 218;
    }
    setScale(m_scale);
}

void BTEItemWidget::setScale(int scale)
{
    if (scale < 1)
        scale = 1;
    const int gap = 3;

    m_scale = scale;
    setFixedSize(m_baseWidth * scale + (scale - 1) * gap, m_baseHeight);
}

JZSize BTEItemWidget::sizeHint() const
{
    // ⚠ 必须是"固定尺寸"本身, 不能返回当前 width(): 那样一旦被压窄, 期望值跟着变小,
    // 布局下一轮会继续压(自反馈)。原版是 setFixedSize, sizeHint 由它推导 —— 等价于此。
    const int gap = 3;
    return JZSize(m_baseWidth * m_scale + (m_scale - 1) * gap, m_baseHeight);
}

JZSize BTEItemWidget::minimumSizeHint() const
{
    return sizeHint();      // 固定尺寸: 最小 == 期望(原版 setFixedSize 的语义)
}

// ---------------------------------------------------------------------------
// 数据 / 状态
// ---------------------------------------------------------------------------
void BTEItemWidget::setItemIndex(int idx)
{
    m_itemIndex = idx;
}

void BTEItemWidget::setChannelText(const std::string& ch)
{
    m_channel = ch;
    update();
}

void BTEItemWidget::setData(const stru_BTS_CMD_SEND_RTDATA76& pack)
{
    m_rtdata = pack;
    updateToolTip();
    update();
}

void BTEItemWidget::setStatus(int status)
{
    m_status = status;
    update();
}

void BTEItemWidget::setAlarmCleared(bool flag)
{
    if (m_alarmCleared == flag)
        return;
    m_alarmCleared = flag;
    update();
}

void BTEItemWidget::setCheck(bool flag)
{
    if (m_checked == flag)
        return;
    m_checked = flag;
    update();
}

void BTEItemWidget::clear()
{
    m_rtdata = stru_BTS_CMD_SEND_RTDATA76();   // 原版: memset(&m_rtdata, 0, ...)
    m_alarmCleared = false;
    update();
}

// ---------------------------------------------------------------------------
// 文案(原版 getDispInfo): 9 项, 第 7/8 项按 StepType 在"充/放电容量·能量"之间切换
// ---------------------------------------------------------------------------
void BTEItemWidget::getDispInfo(std::vector<std::string>& vars)
{
    std::string status = "正常";
    if (m_rtdata.workStatus() == stru_BTS_CMD_SEND_RTDATA76::WORK_ERROR_STOP || m_rtdata.PrtCode != 0)
        status = PackDefine::ErrorText(m_rtdata.PrtCode);

    vars.assign(m_labels.size(), std::string());

    vars[0] = status;
    vars[1] = std::to_string((int)m_rtdata.StepID);
    vars[2] = std::to_string((int)m_rtdata.CycleStatus);
    vars[3] = DataTrans::fromTimeUnitSec(m_rtdata.TestTime);
    vars[4] = num4(DataTrans::fromVoltageUnit(m_rtdata.Voltage));
    vars[5] = num4(DataTrans::fromElectricUnit(m_rtdata.Current));
    vars[6] = num1(DataTrans::fromTempUnit(m_rtdata.Tempature));

    if (m_rtdata.StepType == STEP_TYPE_2)
    {
        m_labels[7] = "放电容量(Ah)";
        m_labels[8] = "放电能量(Wh)";
        vars[7] = num4(DataTrans::fromCapUnit(m_rtdata.DisChargeCap));
        vars[8] = num4(DataTrans::fromEngUnit(m_rtdata.DisChargeEng));
    }
    else
    {
        m_labels[7] = "充电容量(Ah)";
        m_labels[8] = "充电能量(Wh)";
        vars[7] = num4(DataTrans::fromCapUnit(m_rtdata.ChargeCap));
        vars[8] = num4(DataTrans::fromEngUnit(m_rtdata.ChargeEng));
    }
}

// 原版把 9 行汇总成 tooltip(setToolTip)。引擎还没有 tooltip 支持 —— 这里留空,
// 等引擎有了 tooltip 再把上面 getDispInfo 的结果拼进去即可。
void BTEItemWidget::updateToolTip()
{
}

// ---------------------------------------------------------------------------
// 标题(原版 getTitle): 按 status 给文字; 运行中显示工步名(StepNameList)
// ---------------------------------------------------------------------------
void BTEItemWidget::getTitle(std::string& title, JZColor& color) const
{
    color = JZColor(0, 0, 0);

    const int stepType = m_rtdata.StepType;
    switch (m_status)
    {
    case Status_Complete:  title = "完成";     break;
    case Status_WaitStart: title = "正在启动"; break;
    case Status_Run:
        if (stepType == STEP_TYPE_NULL)
            title = "正在启动";
        else if (stepType >= 0 && stepType < (int)PackDefine::StepNameList.size())
            title = PackDefine::StepNameList[stepType];
        else
            title = "未定义";
        break;
    case Status_WaitStop:  title = "正在停止"; break;
    case Status_Stop:      title = "暂停";     break;
    case Status_WaitJump:  title = "正在跳转"; break;
    case Status_Error:     title = "内部错误"; break;
    case Status_OffLine:   title = "离线";     break;
    default:               title.clear();      break;   // 原版这里是 Q_ASSERT(0): 未知状态不画标题
    }
}

// ---------------------------------------------------------------------------
// 底色(原版 bgColor)
// ---------------------------------------------------------------------------
JZColor BTEItemWidget::bgColor() const
{
    if (!isEnabled())
        return JZColor(170, 175, 186);

    if (m_status == Status_Complete)
        return JZColor(192, 192, 192);

    if (m_status == Status_Stop)
    {
        if (m_rtdata.PrtCode != 0)
            return m_alarmCleared ? kFaultClearedBgColor : kFaultBgColor;
        return JZColor(240, 240, 240);
    }

    if (m_status == Status_OffLine && m_rtdata.PrtCode != 0)
        return m_alarmCleared ? kFaultClearedBgColor : kFaultBgColor;

    // 运行中: 按工步类型给底色
    if (m_rtdata.StepType == STEP_TYPE_1 || m_rtdata.StepType == STEP_TYPE_3
        || m_rtdata.StepType == STEP_TYPE_7)
        return JZColor(15, 175, 0);        // 充电: 绿
    if (m_rtdata.StepType == STEP_TYPE_2)
        return JZColor(180, 180, 20);      // 放电: 黄
    if (m_rtdata.StepType == STEP_TYPE_STOP)
        return JZColor(192, 192, 192);
    return JZColor(204, 255, 255);         // 其它: 淡青
}

// ---------------------------------------------------------------------------
// 绘制(原版 paintEvent 逐段对齐)
// ---------------------------------------------------------------------------
void BTEItemWidget::paintEvent(JZPainterEvent*)
{
    JZPainter p(this);

    const int w = width();
    const int h = height();

    JZColor bg_color(192, 192, 192);                  // 原版: 常态灰底
    if (!isEnabled())
        bg_color = JZColor(170, 175, 186);

    JZColor ch_color(128, 64, 0);                     // 通道号颜色
    JZColor status_color(0, 0, 0);

    p.fillRect(0, 0, w, h, bg_color);

    // 边框几何: 四边内缩 6; 顶部中间 80×16 是"凹口"(画通道号)
    const int border_gap = 6;
    const int border_top = border_gap + 1;
    const int border_topw = 80;
    const int border_toph = 16;
    const int border_top2 = border_top + border_toph;
    const int border_top_x1 = (w - border_topw) / 2;
    const int border_top_x2 = border_top_x1 + border_topw;
    const int border_left = border_gap;
    const int border_right = w - border_gap;
    const int border_bottom = h - border_gap;

    // 状态底色: 只填"凹口 + 内容区"(原版 cellColor != bg_color 时)
    const JZColor cell_color = bgColor();
    if (!(cell_color == bg_color))
    {
        p.fillRect(border_top_x1, border_top, border_topw, border_toph, cell_color);
        p.fillRect(border_left, border_top2,
                   border_right - border_left, border_bottom - border_top2, cell_color);
    }

    // 凹陷边框: 上/左深灰, 右/下白(原版两条 setPen)
    p.setPen(JZPen(JZColor(129, 129, 129), 2));
    p.drawLine(border_top_x1, border_top, border_top_x2, border_top);      // 凹口上边
    p.drawLine(border_top_x1, border_top, border_top_x1, border_top2);     // 凹口左竖
    p.drawLine(border_left, border_top2, border_top_x1, border_top2);      // 左边到凹口
    p.drawLine(border_top_x2, border_top2, border_right, border_top2);     // 凹口到右边
    p.drawLine(border_left, border_top2, border_left, border_bottom);      // 左边

    p.setPen(JZPen(JZColor(255, 255, 255), 2));
    p.drawLine(border_top_x2, border_top, border_top_x2, border_top2);     // 凹口右竖
    p.drawLine(border_right, border_top2, border_right, border_bottom);    // 右边
    p.drawLine(border_left, border_bottom, border_right, border_bottom);   // 底边

    // 勾选: 黄色 2px 外框(原版 border.adjust(-2,-2,1,1))
    if (m_checked)
    {
        p.setPen(JZPen(JZColor(255, 255, 0), 2));
        p.drawRect(border_left - 2, border_top - 2,
                   (border_right + 1) - (border_left - 2),
                   (border_bottom + 1) - (border_top - 2));
    }

    // 状态列(报警时红字), 报警/已确认底色上通道号与状态都反成黑色
    std::string status = "正常";
    if (m_rtdata.workStatus() == stru_BTS_CMD_SEND_RTDATA76::WORK_ERROR_STOP || m_rtdata.PrtCode != 0)
    {
        status_color = JZColor(200, 0, 0);
        status = PackDefine::ErrorText(m_rtdata.PrtCode);
    }
    if (cell_color == kFaultBgColor || cell_color == kFaultClearedBgColor)
    {
        status_color = JZColor(0, 0, 0);
        ch_color = JZColor(0, 0, 0);
    }

    std::vector<std::string> vars;
    getDispInfo(vars);

    const int title_y = border_top2 + 24;

    // 通道号: 凹口里居中(14px)
    p.setFont(m_fontTitle);
    p.setPen(JZPen(ch_color, 1));
    {
        const int tw = JZFontMetrics::textWidth(m_channel, m_fontTitle);
        p.drawText(border_top_x1 + (border_topw - tw) / 2,
                   border_top + JZFontMetrics::centerY(m_fontTitle, border_toph),
                   m_channel.c_str());
    }

    // 状态标题: 凹口下方一行, 右对齐(原版 drawText(title_rc, title, AlignBottom|AlignRight))
    {
        std::string title;
        JZColor title_color(0, 0, 0);
        getTitle(title, title_color);
        if (!title.empty())
        {
            const int lineH = title_y - border_top2;
            const int tw = JZFontMetrics::textWidth(title, m_font);
            p.setFont(m_font);
            p.setPen(JZPen(title_color, 1));
            p.drawText(border_right - 20 - tw,
                       border_top2 + JZFontMetrics::centerY(m_font, lineH), title.c_str());
        }
    }

    // 9 行 "标签 + 值"
    int text_x = border_left + 4;
    int text_len = w - text_x - 4;
    if (m_scale != 1)
    {
        text_x = w / 4;
        text_len = w - 2 * text_x;
    }

    const int text_gap = (m_itemSize == Item_Normal)
        ? (text_len / 2 + (text_len / 2 - 60))
        : (text_len / 2 + (text_len / 2 - 45));

    int text_y = title_y + 24;
    p.setFont(m_font);
    for (size_t i = 0; i < m_labels.size(); ++i)
    {
        p.setPen(JZPen(JZColor(0, 0, 0), 1));
        p.drawText(text_x, text_y, m_labels[i].c_str());

        if (i == 0)
        {
            // 第一行(状态)用红字, 放不下就贴右边
            const int vw = JZFontMetrics::textWidth(status, m_font);
            p.setPen(JZPen(status_color, 1));
            if (text_x + text_gap + vw > w)
                p.drawText(w - vw - 8, text_y, status.c_str());
            else
                p.drawText(text_x + text_gap, text_y, status.c_str());
        }
        else
        {
            p.drawText(text_x + text_gap, text_y, vars[i].c_str());
        }

        text_y += (m_itemSize == Item_Normal) ? 22 : 16;   // 原版行距
    }
}

// ---------------------------------------------------------------------------
// 交互(原版 mousePress/Release): 左右键都算按下; 松开时若落回卡片内则勾选
//   · 不带 Ctrl: 变选中 + 发 toggled(true)(原版对"右键点未选中卡"也会选中)
//   · 带 Ctrl : 单纯取反(原版不发信号 —— 多选由上层处理)
// ---------------------------------------------------------------------------
void BTEItemWidget::mousePressEvent(JZMouseEvent* event)
{
    if (event->button() == JZMouseEvent::LeftButton || event->button() == JZMouseEvent::RightButton)
        m_mouseDown = true;

    JZWidget::mousePressEvent(event);
}

void BTEItemWidget::mouseReleaseEvent(JZMouseEvent* event)
{
    const bool inside = rect().contains(event->localPos());
    if (m_mouseDown && inside)
    {
        const bool old_check = m_checked;
        if ((event->modifiers() & JZKeyEvent::CtrlModifier) == 0)
        {
            if (!m_checked)
            {
                m_checked = true;
                update();
            }

            if (event->button() == JZMouseEvent::LeftButton
                || (event->button() == JZMouseEvent::RightButton && !old_check))
                sigToggled.emit(true);
        }
        else
        {
            m_checked = !m_checked;
            update();
        }
    }

    m_mouseDown = false;

    if (event->button() == JZMouseEvent::LeftButton)
        sigClicked.emit();      // 骨架的右键菜单定位用(原版没有这个信号)

    JZWidget::mouseReleaseEvent(event);
}
