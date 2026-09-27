#ifndef JZ_WIDGET_BUTTON_H_
#define JZ_WIDGET_BUTTON_H_
#include "JZWidget.h"
#include "JZSignal.h"
#include "JZImage.h"

class JZFRAMEWORK_API JZPushButton : public JZWidget
{
    JZ_OBJECT_DECLARE(JZPushButton)

public:
    explicit JZPushButton(JZWidget* parent = nullptr);
    JZPushButton(const std::string& text, JZWidget* parent = nullptr);

    JZSize sizeHint() const override;
    JZSize minimumSizeHint() const override;

    void setText(const std::string& text);
    std::string text() const;

    void setIcon(const JZImage& icon);
    const JZImage& icon() const;

    void setWidth(int w);
    void setHeight(int h);

    // ---- AI 语义 ----
    std::string aiRole() const override { return "button"; }
    std::string aiText() const override { return m_text; }
    std::vector<JZAiAction> aiActions() const override { return {{"click", ""}}; }

    // 消费的样式属性: 基类盒子 + 文字色 (能力查询, 故为 public)
    std::vector<std::string> styleProps() const override;

jz_signals:
    JZSigVoid sigClicked;

protected:
    void paintEvent(JZPainterEvent* event) override;

    // UA 默认外观 (等价原来的硬编码): 浅灰底 + 灰边框 + 4px 圆角 + 深灰字
    JZStyleSet uaStyle() const override;
    void mousePressEvent(JZMouseEvent* event) override;
    void mouseReleaseEvent(JZMouseEvent* event) override;

private:
    std::string m_text;
    JZImage m_icon;
    bool m_pressed = false;
};

#endif
