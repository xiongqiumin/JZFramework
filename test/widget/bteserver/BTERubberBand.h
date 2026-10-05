#ifndef BTE_RUBBER_BAND_H_
#define BTE_RUBBER_BAND_H_

#include "JZWidget.h"

// ---------------------------------------------------------------------------
// BTERubberBand —— 框选(橡皮筋)选择层。
//
// 对应原版 BTEServer 里的 `m_selectWidget`(一个 `border: 2px solid rgb(255,255,127)`
// 的 QWidget), 但**触发方式必须不同**:
//   原版靠 QWidget 的事件冒泡 —— 卡片不处理鼠标移动, 事件冒到主窗口, 主窗口的
//   mouseMove/Release 里画框、判命中; 引擎的鼠标事件**不冒泡**
//   (JZWindow::dispatchMouse 是单一命中控件直投), 主窗口收不到在卡片上拖拽的事件。
//   所以这里改成"卡片拖出自身 → 请求窗口 → 窗口用引擎的**鼠标抓取**(grabMouse)
//   把后续鼠标事件直发本控件", 见 BTEItemWidget::sigRubberStart 与
//   BTEServer.cpp 的 initRubberBand / selectCardsInRect。
//
// 本类只做两件事: 自绘选择框、在松开时把框(本控件坐标)与修饰键报出去。
// "哪些卡片被选中"由窗口判定(它才有卡片列表)。
// ---------------------------------------------------------------------------
class BTERubberBand : public JZWidget
{
    JZ_OBJECT_DECLARE(BTERubberBand)

public:
    explicit BTERubberBand(JZWidget* parent = nullptr);

    // 开始一次框选。originGlobal = **按下时**的屏幕坐标(卡片把它传出来):
    // 拖拽第一次 move 时用它反算出选择框的真实起点(见 .cpp 里的校准说明)。
    // 调用方负责: setGeometry(铺满卡片区域) + setVisible(true) + 窗口 grabMouse。
    void begin(const JZPoint& originGlobal);

    // 当前选择框(本控件局部坐标, 已规范化: 左上为原点、宽高非负)
    JZRect selection() const;

protected:
    void paintEvent(JZPainterEvent* event) override;
    void mouseMoveEvent(JZMouseEvent* event) override;
    void mouseReleaseEvent(JZMouseEvent* event) override;

jz_signals:
    // 框选结束(已 hide): modifiers = 松开时的修饰键 —— 调用方据此判断"替换"还是
    // Ctrl"追加"(见 BTEServer::selectCardsInRect)。选择框本身用 selection() 取。
    JZSignal<uint32_t> sigSelected;

private:
    JZPoint m_originGlobal;          // 按下点(屏幕坐标): 校准起点用
    JZPoint m_origin;                // 选择框起点(本控件坐标)
    JZPoint m_current;               // 选择框当前点(本控件坐标)
    bool    m_hasOrigin = false;     // m_origin 是否已校准
};

#endif // BTE_RUBBER_BAND_H_
