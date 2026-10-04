#ifndef JZ_DOUBLE_SPIN_BOX_H_
#define JZ_DOUBLE_SPIN_BOX_H_

#include "JZSpinBox.h"

// ==================== 小数数字步进框 (对齐 Qt QDoubleSpinBox) ====================
// 复用 JZSpinBox 的全部绘制/编辑/步进交互(按钮、滚轮、上下键、键入提交), 只把这些换成小数口径:
// 值 `double`、范围/步长 `double`、显示精度 `decimals`、前缀 `prefix`。
//
// ⚠ 信号: 本类声明了与基类**同名**的 `sigValueChanged`, 但类型是 JZSigDouble(基类是 JZSigInt)。
//   * 在本类作用域(以及 JZDoubleSpinBox 对象上)取 `sigValueChanged` ⇒ 拿到 double 那个;
//   * 通过 `JZSpinBox*` 取 ⇒ 拿到基类的 int 信号(基类指针的调用方本来按整数口径用, 这是有意的)。
//   所以连槽时直接对本类对象取该成员, 别经基类指针转手。
//
// 注: 基类的 setStep/step 是整数口径(内部按 >=1 夹取), 小数步长请用 setSingleStep/singleStep。
class JZFRAMEWORK_API JZDoubleSpinBox : public JZSpinBox
{
    JZ_OBJECT_DECLARE(JZDoubleSpinBox)

public:
    explicit JZDoubleSpinBox(JZWidget* parent = nullptr);

    // ---- 值 / 范围 / 步长(Qt 同名) ----
    void setValue(double v);
    double value() const;

    void setRange(double min, double max);
    void setMinimum(double min);
    double minimum() const;
    void setMaximum(double max);
    double maximum() const;

    void setSingleStep(double step);      // Qt: singleStep(double); 非正值忽略(保持原步长)
    double singleStep() const;

    // ---- 显示 ----
    void setDecimals(int n);              // 显示小数位(0..6, Qt 默认 2)
    int  decimals() const;
    void setPrefix(const std::string& s);
    std::string prefix() const;
    // 后缀直接用基类的 setSuffix/suffix()

    // 注: AI 语义(aiRangeValue/aiActions)沿用基类的整数口径实现 —— 小数值会被取整上报,
    //     AI 工具要精确值请读 value()。暂不为小数单开一套 AI 语义。

jz_signals:
    JZSigDouble sigValueChanged;   // (值)

protected:
    std::string valueText() const override;                              // 按 decimals 格式化 + 前后缀
    bool parseEditText(const std::string& buf, double& out) const override;
    bool isEditableChar(char c) const override;                          // 多认 '.' 和 '-'
    void emitValueChanged() override;                                    // 发 double 信号

private:
    int m_decimals = 2;
    std::string m_prefix;
};

#endif // JZ_DOUBLE_SPIN_BOX_H_
