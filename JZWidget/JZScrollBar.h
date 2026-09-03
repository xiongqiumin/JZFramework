#ifndef JZ_SCROLLBAR_H_
#define JZ_SCROLLBAR_H_

#include "JZWidget.h"
#include "JZSignal.h"

// 独立滚动条 (类似 Qt QScrollBar): 沿一个方向排布, 滑块长度正比于 pageStep,
// 两端箭头单步、轨道点击翻页、滑块可拖动、滚轮单步。可挂载到任意可滚动内容
// (如自定义 JZScrollArea 子类) 上, 由外部在 sigValueChanged 里同步滚动偏移。
class JZScrollBar : public JZWidget
{
    JZ_OBJECT_DECLARE(JZScrollBar)

public:
    enum Orientation {
        Horizontal,   // 沿 x 轴
        Vertical      // 沿 y 轴
    };

    explicit JZScrollBar(Orientation o = Horizontal, JZWidget* parent = nullptr);

    JZSize sizeHint() const override;

    void setOrientation(Orientation o);
    Orientation orientation() const;

    void setRange(int min, int max);
    void setMinimum(int min);
    int  minimum() const;
    void setMaximum(int max);
    int  maximum() const;
    void setValue(int v);
    int  value() const;
    void setSingleStep(int step);
    int  singleStep() const;
    void setPageStep(int step);
    int  pageStep() const;

jz_signals:
    JZSigInt sigValueChanged;   // (value)

protected:
    void paintEvent(JZPainterEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;
    void mouseMoveEvent(JZMouseEvent* event) override;
    void mouseReleaseEvent(JZMouseEvent* event) override;
    void wheelEvent(JZMouseEvent* event) override;

private:
    int  range() const { return m_max - m_min; }
    int  grooveStart() const;     // 凹槽起点(沿主轴, 扣两端箭头)
    int  grooveLen() const;       // 凹槽长度(沿主轴)
    int  thumbLen() const;        // 滑块长度(沿主轴, 正比 pageStep)
    int  valueToPos(int value) const;   // value -> 滑块起点(沿主轴)
    int  posToValue(int pos) const;     // 滑块起点(沿主轴) -> value
    int  hitTest(const JZPoint& p) const; // -1 减箭头 0 前凹槽 1 滑块 2 后凹槽 3 加箭头

    Orientation m_orient = Horizontal;
    int m_min = 0;
    int m_max = 100;
    int m_value = 0;
    int m_singleStep = 1;
    int m_pageStep = 10;
    int m_arrow = 16;      // 两端箭头按钮长度(沿主轴)
    int m_thumbMin = 16;   // 滑块最小长度
    bool m_dragging = false;
    int  m_dragGrab = 0;   // 抓取时: 点击位置 - 滑块起点
    int  m_pressCode = -2; // 当前按下的区域(用于高亮), -2=无
};

#endif // JZ_SCROLLBAR_H_
