#ifndef JZ_SPIN_BOX_H_
#define JZ_SPIN_BOX_H_
#include <string>
#include "JZWidget.h"
#include "JZSignal.h"

// 整数数字步进框 (类似 Qt QSpinBox): 左右为编辑区, 右侧上下两个按钮;
// 支持按钮/滚轮/方向键步进, 聚焦后可直接键入数字 (回车提交, Esc 取消)。
//
// 与 JZDoubleSpinBox 的关系: 本类是"整数口径"的那一支; JZDoubleSpinBox 派生自它, 复用全部
// 绘制/编辑/步进逻辑, 只把"文本表示 / 编辑解析 / 可键入字符 / 值信号类型"换掉 —— 为此下面
// 留了一组 protected 虚钩子(valueText/parseEditText/isEditableChar/emitValueChanged)。
// 注: 值的内部存储是 double(整数语义不变), 好让派生类直接拿它做小数运算与夹取。
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
    // 值处于**最小值**时显示这段文本(对齐 Qt QSpinBox/QDoubleSpinBox::specialValueText,
    // 常用于"0 显示为 未设置"); 空串 = 不启用。派生类(JZDoubleSpinBox)同样生效。
    void setSpecialValueText(const std::string& s);
    std::string specialValueText() const;
    void setReadOnly(bool ro);
    bool isReadOnly() const;

    // ---- AI 语义 ----
    bool aiRangeValue(int& value, int& min, int& max) const override
    { value = (int)m_value; min = (int)m_min; max = (int)m_max; return true; }
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

    // ==== 给 JZDoubleSpinBox 的钩子(基类实现一律按整数口径) ====
    // 编辑区显示的文本(含前缀/后缀)
    virtual std::string valueText() const;
    // 编辑缓冲 -> 值; 解析失败返回 false(调用方丢弃缓冲, 回退原值)
    virtual bool parseEditText(const std::string& buf, double& out) const;
    // 可键入的字符(基类只认数字)
    virtual bool isEditableChar(char c) const;
    // **所有**内部改值都走它(夹取 + 存 + 发信号 + 重绘); 注意别在内部直接调 setValue(int),
    // 那样在派生类对象上会静态派发到基类的整数版本, 把小数截断。
    virtual void setValueInternal(double v);
    // 值信号(基类发 int; JZDoubleSpinBox 覆盖它以发 double)
    virtual void emitValueChanged();

    // 派生类要直接算/夹取, 所以数据成员放 protected(数值一律 double 存储)
    double m_min = 0;
    double m_max = 100;
    double m_step = 1;
    double m_value = 0;
    std::string m_suffix;
    std::string m_special;   // 最小值时显示的文本(Qt: specialValueText)

private:
    void commitEdit();     // 解析编辑缓冲并提交
    void cancelEdit();     // 放弃编辑
    int  hitButton(const JZPoint& p) const; // 0 上, 1 下, -1 无

    bool m_readOnly = false;
    bool m_pressUp = false;
    bool m_pressDown = false;
    bool m_editing = false;
    std::string m_editBuffer;
};

#endif // JZ_SPIN_BOX_H_
