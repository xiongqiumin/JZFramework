#ifndef JZ_WIDGET_LABEL_H_
#define JZ_WIDGET_LABEL_H_

#include "JZWidget.h"

class JZLabel : public JZWidget
{
    JZ_OBJECT_DECLARE(JZLabel)

public:
    explicit JZLabel(JZWidget* parent = nullptr);
    JZLabel(const std::string& text, JZWidget* parent = nullptr);

    JZSize sizeHint() const override;
    JZSize minimumSizeHint() const override;
    void setText(const std::string& text);
    std::string text() const;

    void setAlign(int align);

    // ---- AI 语义 ----
    std::string aiText() const override { return m_text; }

    // 消费的样式属性: 基类盒子 + 文字色 (能力查询, 故为 public)
    std::vector<std::string> styleProps() const override;

protected:
    void paintEvent(JZPainterEvent* event) override;

    // UA 默认外观: 文字 #333333
    JZStyleSet uaStyle() const override;

private:
    std::string m_text;
    int m_align = 0;   // 0=左, 1=中, 2=右
};

#endif
