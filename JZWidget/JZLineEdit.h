#ifndef JZ_LINE_EDIT_H_
#define JZ_LINE_EDIT_H_

#include "JZAbstractTextEdit.h"

// ==================== 单行文本输入框 ====================
// 全部编辑/绘制/输入法逻辑在 JZAbstractTextEdit (单行模式), 本类只提供单行语义:
// 密码框、最大长度、回车/失焦 "完成编辑" 的信号含义与默认尺寸。
class JZLineEdit : public JZAbstractTextEdit
{
    JZ_OBJECT_DECLARE(JZLineEdit)

public:
    explicit JZLineEdit(JZWidget* parent = nullptr);

    JZSize sizeHint() const override;

    void setPassword(bool password);    // 显示为 '*'
    bool isPassword() const;

    void setMaxLength(int n);           // 0 = 不限
    int  maxLength() const;
};

#endif // JZ_LINE_EDIT_H_
