#ifndef JZ_SPIN_BOX_H_
#define JZ_SPIN_BOX_H_
#include <string>
#include "JZWidget.h"
#include "JZSignal.h"

// 整数数字步进框 (类似 Qt QSpinBox): 左右为编辑区, 右侧上下两个按钮;
// 支持按钮/滚轮/方向键步进, 聚焦后可直接键入数字 (回车提交, Esc 取消)。
class JZFRAMEWORK_API JZSpinBox : public JZWidget
{
    JZ_OBJECT_DECLARE(JZSpinBox)

public:
    explicit JZSpinBox(JZWidget* parent = nullptr);
    JZSize sizeHint() const override;
    JZSize minimumSizeHint() const override;

    void setRange(int min, int max);
    void setMinimum(int min);
    int  minimum() const;
    void setMaximum(int max);
    int  maximum() const;
    void setStep(int step);
    int  step() const;
    void setValue(int v);
    int  value() const;
    void setSuffix(const std::string& s);
    std::string suffix() const;
    void setReadOnly(bool ro);
    bool isReadOnly() const;

    // ---- AI 语义 ----
    bool aiRangeValue(int& value, int& min, int& max) const override
    { value = m_value; min = m_min; max = m_max; return true; }
    std::vector<JZAiAction> aiActions() const override { return {{"setValue", "number 新值"}}; }

jz_signals:
    JZSigInt sigValueChanged;

protected:
    void paintEvent(JZPainterEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;
    void mouseReleaseEvent(JZMouseEvent* event) override;
    void wheelEvent(JZMouseEvent* event) override;
    void keyPressEvent(JZKeyEvent* event) override;
    void focusInEvent(JZEvent* event) override;
    void focusOutEvent(JZEvent* event) override;

private:
    void commitEdit();     // 解析编辑缓冲并提交
    void cancelEdit();     // 放弃编辑
    int  hitButton(const JZPoint& p) const; // 0 上, 1 下, -1 无

    int m_min = 0;
    int m_max = 100;
    int m_step = 1;
    int m_value = 0;
    std::string m_suffix;
    bool m_readOnly = false;
    bool m_pressUp = false;
    bool m_pressDown = false;
    bool m_editing = false;
    std::string m_editBuffer;
};

#endif // JZ_SPIN_BOX_H_
