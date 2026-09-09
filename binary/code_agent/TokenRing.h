#ifndef JZ_CODE_AGENT_TOKENRING_H_
#define JZ_CODE_AGENT_TOKENRING_H_

#include "JZWidget.h"
#include "JZString.h"

// 输入框内的 token 用量圆环：环上按比例绘制进度弧，鼠标移到环上走 JZWidget 通用
// setTooltip 弹出悬浮窗, 给出"已用 / 上限 / 剩余"的具体数字（按 K 折算、单行）。
// 只负责展示, 数据由主窗口定时刷新。
class TokenRing : public JZWidget
{
    JZ_OBJECT_DECLARE(TokenRing)
public:
    explicit TokenRing(JZWidget* parent = nullptr);

    // used = 当前已用 token，total = 上限（<=0 时只画空环）
    void setUsage(int used, int total);

protected:
    void paintEvent(JZPainterEvent* event) override;

private:
    int     percent() const;
    JZString buildTooltip() const;

    int m_used = 0;
    int m_total = 0;
};

#endif // !JZ_CODE_AGENT_TOKENRING_H_
