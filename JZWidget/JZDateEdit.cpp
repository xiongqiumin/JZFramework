// JZDateEdit —— 日期编辑框。**接口先落地, 实现留空**(见 JZDateEdit.h 的说明)。
// 下面这些是"能安全做的最小实现": 日期模型 + 范围夹取 + 按格式写文本 + 发信号。
// 明确留 TODO 的: 文本->日期解析、键盘/滚轮微调、弹出日历。
#include "JZDateEdit.h"

JZ_OBJECT_IMPL(JZDateEdit, JZLineEdit)

JZDateEdit::JZDateEdit(JZWidget* parent)
    : JZLineEdit(parent)
    , dateChanged(this)
{
    // Qt 的 QDateEdit 默认带一个初始日期(这里是"今天"); 调用方一般会显式 setDate。
    m_date = JZDateTime::currentDateTime();
    applyTextFromDate();
}

JZSize JZDateEdit::sizeHint() const
{
    // TODO(实现): 日期框通常比普通输入框窄一点/固定宽; 先直接沿用单行框的尺寸提示。
    return JZLineEdit::sizeHint();
}

// ---------------------------------------------------------------------------
// 日期 / 范围
// ---------------------------------------------------------------------------
JZDateTime JZDateEdit::clampToRange(const JZDateTime& d) const
{
    JZDateTime v = d;
    if (m_hasMin && v.toMSecsSinceEpoch() < m_min.toMSecsSinceEpoch())
        v = m_min;
    if (m_hasMax && v.toMSecsSinceEpoch() > m_max.toMSecsSinceEpoch())
        v = m_max;
    return v;
}

void JZDateEdit::setDate(const JZDateTime& d)
{
    const JZDateTime v = clampToRange(d);
    if (v.toMSecsSinceEpoch() == m_date.toMSecsSinceEpoch())
        return;   // 值没变不发信号(Qt 同口径)

    m_date = v;
    applyTextFromDate();
    dateChanged.emit(m_date);
}

JZDateTime JZDateEdit::date() const
{
    return m_date;
}

void JZDateEdit::setMinimumDate(const JZDateTime& d)
{
    m_min = d;
    m_hasMin = true;
    setDate(m_date);   // 新范围可能把当前值夹住
}

JZDateTime JZDateEdit::minimumDate() const
{
    return m_min;
}

bool JZDateEdit::hasMinimumDate() const
{
    return m_hasMin;
}

void JZDateEdit::setMaximumDate(const JZDateTime& d)
{
    m_max = d;
    m_hasMax = true;
    setDate(m_date);
}

JZDateTime JZDateEdit::maximumDate() const
{
    return m_max;
}

bool JZDateEdit::hasMaximumDate() const
{
    return m_hasMax;
}

void JZDateEdit::clearMinimumDate()
{
    m_hasMin = false;
}

void JZDateEdit::clearMaximumDate()
{
    m_hasMax = false;
}

// ---------------------------------------------------------------------------
// 格式 / 弹出日历 / 微调
// ---------------------------------------------------------------------------
void JZDateEdit::setDisplayFormat(const std::string& format)
{
    if (format.empty() || format == m_format)
        return;

    m_format = format;
    applyTextFromDate();
}

std::string JZDateEdit::displayFormat() const
{
    return m_format;
}

void JZDateEdit::setCalendarPopup(bool on)
{
    // TODO(实现): 真正做弹出日历需要 JZCalendarWidget(尚未做)。
    //   Qt 语义: 点击右侧按钮/下拉弹出月历, 选中的日期回写 setDate()。
    m_calendarPopup = on;
}

bool JZDateEdit::calendarPopup() const
{
    return m_calendarPopup;
}

void JZDateEdit::stepBy(int days)
{
    if (days == 0)
        return;
    setDate(m_date.addDays(days));
}

// ---------------------------------------------------------------------------
// 内部
// ---------------------------------------------------------------------------
void JZDateEdit::applyTextFromDate()
{
    // TODO(实现): 与 displayFormat 对应的 Qt 字母表要跟 JZDateTime::toString 对齐核一遍;
    //   另外文本框应当是"只读文本 + 上下微调"的观感(Qt 的 QDateEdit 其实可编辑, 但键入要走解析)。
    setText(m_date.toString(m_format));
}
