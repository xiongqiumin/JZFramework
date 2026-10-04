#include "JZDoubleSpinBox.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

JZ_OBJECT_IMPL(JZDoubleSpinBox, JZSpinBox)

JZDoubleSpinBox::JZDoubleSpinBox(JZWidget* parent)
    : JZSpinBox(parent)
    , sigValueChanged(this)
{}

// ==================== 值 / 范围 / 步长 ====================
void JZDoubleSpinBox::setValue(double v)
{
    setValueInternal(v);   // 夹取/存/发信号/重绘都在基类里, 这里走的是本类覆盖的 emitValueChanged
}

double JZDoubleSpinBox::value() const
{
    return m_value;
}

void JZDoubleSpinBox::setRange(double min, double max)
{
    m_min = min;
    m_max = max;
    setValueInternal(m_value);   // 新范围可能夹住当前值
}

void JZDoubleSpinBox::setMinimum(double min) { setRange(min, m_max); }
double JZDoubleSpinBox::minimum() const { return m_min; }
void JZDoubleSpinBox::setMaximum(double max) { setRange(m_min, max); }
double JZDoubleSpinBox::maximum() const { return m_max; }

void JZDoubleSpinBox::setSingleStep(double step)
{
    if (step <= 0.0)
        return;   // 非正值忽略(Qt 也要求 > 0)
    m_step = step;
}

double JZDoubleSpinBox::singleStep() const { return m_step; }

// ==================== 显示 ====================
void JZDoubleSpinBox::setDecimals(int n)
{
    const int nn = std::max(0, std::min(n, 6));
    if (nn == m_decimals)
        return;
    m_decimals = nn;
    update();
}

int JZDoubleSpinBox::decimals() const { return m_decimals; }

void JZDoubleSpinBox::setPrefix(const std::string& s)
{
    if (s == m_prefix)
        return;
    m_prefix = s;
    update();
}

std::string JZDoubleSpinBox::prefix() const { return m_prefix; }

// ==================== 覆盖的钩子 ====================
std::string JZDoubleSpinBox::valueText() const
{
    // Qt 语义: 值 == 最小值 且设了 specialValueText 时显示那段文本(基类的接口/成员都可用)
    if (!specialValueText().empty() && m_value == m_min)
        return specialValueText();

    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", m_decimals, m_value);
    return m_prefix + buf + suffix();
}

bool JZDoubleSpinBox::parseEditText(const std::string& buf, double& out) const
{
    if (buf.empty())
        return false;
    char* end = nullptr;
    const double v = std::strtod(buf.c_str(), &end);
    if (end == buf.c_str())
        return false;   // 一个字符都没吃进去(例如全是 '-' 或 '.')
    out = v;
    return true;
}

bool JZDoubleSpinBox::isEditableChar(char c) const
{
    return std::isdigit((unsigned char)c) != 0 || c == '.' || c == '-';
}

void JZDoubleSpinBox::emitValueChanged()
{
    sigValueChanged.emit(m_value);   // 本类的 double 信号(基类同名成员是 int 那个)
}
