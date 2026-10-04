#ifndef JZ_DATE_EDIT_H_
#define JZ_DATE_EDIT_H_

#include "JZLineEdit.h"
#include "JZDateTime.h"

// ==================== 日期编辑框 (对齐 Qt QDateEdit) ====================
// 现状(2026-10-04): **接口先落地, 实现留空** —— 底子是 JZLineEdit(单行编辑/输入法/绘制都现成),
// 日期模型只是本类里的一个 JZDateTime; 下面这些还没有真实现(见 .cpp 的 TODO):
//   * 文本 <-> 日期的**解析**(键入 "2026-10-04" 回写 m_date): JZDateTime 只有 toString(format),
//     没有 fromString —— 要补解析(或给 JZDateTime 加 fromString);
//   * 键盘上下 / 滚轮微调(现在只有编程接口 stepBy);
//   * 弹出日历(calendarPopup): 需要 JZCalendarWidget, 尚未做 ⇒ setCalendarPopup 目前只记状态。
//
// 为什么先把接口立起来: BTEServer 的 BTEServer.ui 里有 2 个 QDateEdit("从/至"时间查询), TS 移植时
// 因为"引擎没有日期控件"被转换器降级成 LineEdit(见 BTEServer.ui.report.txt), setDate()/date()
// 这些调用点只能靠兼容层兜。C++ 版 bteserver 照原版 C++ 写, 所以类要先在。
//
// 用法(照 Qt 写就行):
//     auto* ed = new JZDateEdit(parent);
//     ed->setDisplayFormat("yyyy-MM-dd");
//     ed->setDate(JZDateTime(2026, 10, 4));
//     connect(&ed->dateChanged, this, &Xxx::onDateChanged);
class JZFRAMEWORK_API JZDateEdit : public JZLineEdit
{
    JZ_OBJECT_DECLARE(JZDateEdit)

public:
    explicit JZDateEdit(JZWidget* parent = nullptr);

    JZSize sizeHint() const override;

    // ---- 日期 ----
    void setDate(const JZDateTime& d);
    JZDateTime date() const;

    // ---- 范围: Qt 的 setMinimumDate/setMaximumDate。默认未设置(不做夹取) ----
    void setMinimumDate(const JZDateTime& d);
    JZDateTime minimumDate() const;
    bool hasMinimumDate() const;

    void setMaximumDate(const JZDateTime& d);
    JZDateTime maximumDate() const;
    bool hasMaximumDate() const;

    void clearMinimumDate();
    void clearMaximumDate();

    // ---- 显示格式: 与 Qt 同名同义("yyyy-MM-dd"), 字母表沿用 JZDateTime::toString 的口径 ----
    void setDisplayFormat(const std::string& format);
    std::string displayFormat() const;

    // ---- 弹出日历(Qt: setCalendarPopup)。JZCalendarWidget 还没做 ⇒ 现在只记录状态 ----
    void setCalendarPopup(bool on);
    bool calendarPopup() const;

    // ---- 微调(对应 Qt 的键盘上下/滚轮): 正数往后, 负数往前 ----
    void stepBy(int days);

jz_signals:
    JZSigDateTime dateChanged;   // 日期真的变了才发(值相同不发)

protected:
    // 按 displayFormat 把 m_date 写进文本框(实现里已做: 只是 setText)
    void applyTextFromDate();
    // 夹到 [min,max](未设置的一边不夹)
    JZDateTime clampToRange(const JZDateTime& d) const;

private:
    JZDateTime m_date;
    JZDateTime m_min;
    JZDateTime m_max;
    bool m_hasMin = false;
    bool m_hasMax = false;
    std::string m_format = "yyyy-MM-dd";
    bool m_calendarPopup = false;
};

#endif // !JZ_DATE_EDIT_H_
