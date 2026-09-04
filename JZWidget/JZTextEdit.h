#ifndef JZ_WIDGET_TEXTAREA_H_
#define JZ_WIDGET_TEXTAREA_H_

#include "JZAbstractTextEdit.h"

// ==================== 多行文本编辑框 ====================
// 全部编辑/绘制/输入法逻辑在 JZAbstractTextEdit (多行模式), 本类只提供多行语义:
// 自动换行开关与默认尺寸。
class JZTextEdit : public JZAbstractTextEdit
{
    JZ_OBJECT_DECLARE(JZTextEdit)

public:
    explicit JZTextEdit(JZWidget* parent = nullptr);

    JZSize sizeHint() const override;

    // 自动换行; 关闭时超出宽度的内容靠水平滚动查看
    void setLineWrap(bool wrap);
    bool lineWrap() const;
};

#endif
