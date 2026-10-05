#ifndef BTEITEMWIDGET_H_
#define BTEITEMWIDGET_H_

#include "JZWidget.h"
#include "JZSignal.h"
#include "JZFont.h"
#include "JZColor.h"
#include "JZPoint.h"
#include "PackDefine.h"    // stru_BTS_CMD_SEND_RTDATA76(卡片吃实时帧)

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// BTEItemWidget —— 一个**通道卡片**(原版 BTEServer/BTEItemWidget.{h,cpp} 的自绘 QWidget)。
//
// 原版是 440 行自绘, 这里逐段照搬它的 paintEvent / bgColor / getTitle / getDispInfo,
// 只做 Qt -> 引擎 的 API 替换:
//   QPainter/QPen/QColor/QFont            -> JZPainter/JZPen/JZColor/JZFont
//   QFontMetrics::width / QRect+Align     -> JZFontMetrics::textWidth / 自己算居中·右对齐
//   QApplication::keyboardModifiers()     -> JZMouseEvent::modifiers()
//   setToolTip                            -> 引擎还没有 tooltip, updateToolTip() 留空(见 .cpp)
//
// 卡片长什么样(照原版):
//   灰底(192,192,192) + "顶部凹口"的立体边框(上/左深灰, 右/下白) + 凹口里居中画通道号(14px)
//   + 右对齐的状态标题 + 9 行"标签: 值"(状态/工步序号/循环序号/工步时间/电压/电流/温度/容量/能量)
//   + 勾选时黄色外框; 底色按 status/StepType 变(充电绿 / 放电黄 / 其它淡青 / 故障红 / 已确认橙)。
//
// 信号: sigToggled(bool)(原版 signals: toggled(bool)) / sigClicked(void)(骨架给右键菜单用)
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

    void setChannelText(const std::string& ch);
    const std::string& channelText() const { return m_channel; }

    stru_BTS_CMD_SEND_RTDATA76 data() const { return m_rtdata; }
    void setData(const stru_BTS_CMD_SEND_RTDATA76& pack);

    int  status() const { return m_status; }
    void setStatus(int status);                 // 取值见 Common.h 的 DeviceStatus

    // 报警确认标志: 该通道报错被"清除报警"确认过后, 故障卡片由红色改为橙色(原版同名接口)
    void setAlarmCleared(bool flag);
    bool isAlarmCleared() const { return m_alarmCleared; }

    int  scale() const { return m_scale; }
    void setScale(int scale);

    void clear();
    void getTitle(std::string& title, JZColor& color) const;   // 原版 getTitle(QString&, QColor&)

    void setCheck(bool flag);
    bool isChecked() const { return m_checked; }

    void setSize(int itemSize);
    int  itemSize() const { return m_itemSize; }

jz_signals:
    JZSigVoid sigClicked;      // 左键点卡片(骨架用于右键菜单定位, 原版没有)
    JZSigBool sigToggled;      // 勾选态变化(带新值)

    // 按下后拖出自身超过 40px(原版 BTEServer::mouseMoveEvent 的阈值)= 用户想框选。
    // 参数是**按下点的屏幕坐标**(选择框起点要用, 见 BTERubberBand::begin)。
    // 原版靠事件冒泡把拖拽交给主窗口; 引擎的鼠标事件不冒泡(单一命中控件直投),
    // 所以卡片主动报出来, 由窗口开始框选(见 BTEServer::initRubberBand)。
    JZSignal<JZPoint> sigRubberStart;

protected:
    void paintEvent(JZPainterEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;
    void mouseMoveEvent(JZMouseEvent* event) override;
    void mouseReleaseEvent(JZMouseEvent* event) override;

private:
    void getDispInfo(std::vector<std::string>& vars);   // 非 const: 原版在这里按 StepType 换"充/放电容量"标签
    void updateToolTip();
    JZColor bgColor() const;

    std::vector<std::string> m_labels;      // 9 项标签(第 7/8 项按 StepType 变)
    JZFont      m_font;                     // 数值行: 12px(Normal) / 9px(Small)
    JZFont      m_fontTitle;                // 通道号: 14px
    std::string m_channel;

    int  m_itemIndex = -1;
    int  m_status = 0;                      // DeviceStatus(默认 Status_Complete = 0)
    bool m_alarmCleared = false;
    stru_BTS_CMD_SEND_RTDATA76 m_rtdata;

    bool m_mouseDown = false;
    bool m_checked = false;

    // ---- 框选拖拽 (见 sigRubberStart) ----
    JZPoint m_pressGlobal;          // 按下点(屏幕坐标): 判"拖出 40px"用
    int     m_pressButton = 0;      // 按下的键(JZMouseEvent::Button): 只有左键能起框选
    bool    m_dragged = false;      // 本次按下是否已判为拖拽(拖过就不当点击处理)
    int  m_scale = 1;
    int  m_baseWidth = 170;
    int  m_baseHeight = 272;
    int  m_itemSize = Item_Normal;
};

#endif // BTEITEMWIDGET_H_
