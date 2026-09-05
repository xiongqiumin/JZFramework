#ifndef JZ_WIDGET_SLIDER_H_
#define JZ_WIDGET_SLIDER_H_

#include "JZWidget.h"
#include "JZSignal.h"

class JZSlider : public JZWidget
{
    JZ_OBJECT_DECLARE(JZSlider)

public:
    explicit JZSlider(JZWidget* parent = nullptr);

    JZSize sizeHint() const override;
    void setRange(int32_t min, int32_t max);
    void setValue(int32_t v);
    int32_t value() const;

    // ---- AI 语义 ----
    bool aiRangeValue(int& value, int& min, int& max) const override
    { value = m_value; min = m_min; max = m_max; return true; }
    std::vector<JZAiAction> aiActions() const override { return {{"setValue", "number 新值"}}; }

jz_signals:
    JZSigInt sigValueChanged;

protected:
    void paintEvent(JZPainterEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;
    void mouseMoveEvent(JZMouseEvent* event) override;
    void mouseReleaseEvent(JZMouseEvent* event) override;

private:
    int valueFromPos(int x) const;   // 命中位置 -> 值 (与 paintEvent 的 6px 边距对齐)
    int knobCenterX() const;         // 滑块圆钮中心 x (与 paintEvent 的 6px 边距对齐)
    void updateHoverCursor(const JZPoint& localPos);   // 悬停/松手后按位置刷新光标

    int32_t m_min = 0;
    int32_t m_max = 100;
    int32_t m_value = 0;
    bool m_dragging = false;
};

#endif
