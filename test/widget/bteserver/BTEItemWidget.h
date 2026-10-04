#ifndef BTEITEMWIDGET_H_
#define BTEITEMWIDGET_H_

#include "JZWidget.h"
#include "JZSignal.h"
#include "JZFont.h"

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// BTEItemWidget —— 一个**通道卡片**(原版 BTEServer/BTEItemWidget.h/.cpp 的自绘控件)。
//
// 原版是一个 500+ 行的自绘 QWidget: 背景色/边框/标题/若干数值行/勾选态/右键菜单, 还带
// 一整套"报警未清除=红、已确认=橙"的配色与 tooltip 汇总。这里按移植版(BTEItemWidget.ts)
// 的**接口面**做一个骨架子集, 够 4x8 网格跑起来 + 勾选/信号联动:
//
//   setItemIndex/itemIndex      卡片序号(0..MAX_DEVICE_ITEM-1)
//   setChannelText/channelText  标题(原版是 "单元-通道", 如 "01-01")
//   setCheck/isChecked          勾选态; 左键切换并发 sigToggled(bool)
//   setAlarm/setAlarmCleared    报警配色(未清除红 / 已确认橙) —— 原版 FaultBgColor/FaultClearedBgColor
//   setSize/itemSize            Item_Normal / Item_Small(只影响卡片尺寸)
//   setScale                    缩放(原版设置里的缩放系数; 这里只影响尺寸)
//   setValueLines               卡片上的数值行(骨架用纯文本行; 原版是 4 组"标签+值+单位")
//   clear()                     复位(序号/标题/值/勾选/报警)
//
// 未做(骨架明确留的): 真正的实时数据包(stru_BTS_CMD_SEND_RTDATA76)解析与 9 项数值/单位、
// tooltip 汇总(getDispInfo/updateToolTip)、Item_Small 的紧凑布局细节、Ctrl 多选与框选。
//
// 信号: sigClicked(void) / sigToggled(bool) —— 继承来的 sigCustomContextMenuRequested(x,y)
// 由框架的右键流程发(见 JZWidget 的 ContextMenuPolicy)。
// ---------------------------------------------------------------------------
class BTEItemWidget : public JZWidget
{
    JZ_OBJECT_DECLARE(BTEItemWidget)

public:
    enum ItemSize { Item_Normal = 0, Item_Small = 1 };

    explicit BTEItemWidget(JZWidget* parent = nullptr);

    JZSize sizeHint() const override;
    JZSize minimumSizeHint() const override;

    void setItemIndex(int idx);
    int  itemIndex() const { return m_itemIndex; }

    void setChannelText(const std::string& text);
    std::string channelText() const { return m_channelText; }

    void setCheck(bool flag);
    bool isChecked() const { return m_checked; }

    void setAlarm(bool on);                 // 报警(未清除 -> 红底)
    bool isAlarm() const { return m_alarm; }
    void setAlarmCleared(bool on);          // 报警已确认 -> 橙底
    bool isAlarmCleared() const { return m_alarmCleared; }

    void setSize(ItemSize s);
    ItemSize itemSize() const { return m_size; }

    // 缩放 = 卡片占用的列数(原版是"并联通道数"): 卡片**固定尺寸** —— 原版
    // setScale() 里唯一一次 setFixedSize(baseWidth*scale + (scale-1)*gap, baseHeight),
    // baseWidth 170(Normal)/120(Small), baseHeight 272/218, gap 3。尺寸与窗口宽度无关,
    // 8 列放不下时由外层滚动区出滚动条(见 .ui 的 cardScroll)。
    void setScale(int scale);               // <1 按 1
    int scale() const { return m_scale; }
    JZSize fixedSize() const;               // 当前 ItemSize/scale 下的固定尺寸

    // 卡片上的数值行(骨架用纯文本; 原版是"标签 + 值 + 单位"的 9 项)
    void setValueLines(const std::vector<std::string>& lines);
    const std::vector<std::string>& valueLines() const { return m_lines; }

    void clear();

    std::string getTitle() const;           // 原版是"通道文本 + 标题"

jz_signals:
    JZSigVoid sigClicked;                   // 左键点卡片
    JZSigBool sigToggled;                   // 勾选态变化(带新值)

protected:
    void paintEvent(JZPainterEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;
    void mouseReleaseEvent(JZMouseEvent* event) override;
    void enterEvent(JZEvent* event) override;
    void leaveEvent(JZEvent* event) override;

private:
    int         m_itemIndex = -1;
    std::string m_channelText;
    std::vector<std::string> m_lines;
    bool        m_checked = false;
    bool        m_alarm = false;
    bool        m_alarmCleared = false;
    bool        m_pressed = false;
    bool        m_hover = false;
    ItemSize    m_size = Item_Normal;
    int         m_scale = 1;       // 占用列数(原版并联通道数)
};

#endif // BTE_ITEM_WIDGET_H_
