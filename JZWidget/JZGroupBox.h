#ifndef JZ_GROUP_BOX_H_
#define JZ_GROUP_BOX_H_

#include <string>
#include "JZWidget.h"

// 带标题的分组框容器 (类似 Qt QGroupBox): 绘制带缺口的边框 + 标题,
// 内容放在内部 contentWidget() 上 (布局也转发到它)。用法:
//   JZGroupBox* gb = new JZGroupBox("Settings", parent);
//   gb->contentWidget()->setLayout(new JZVBoxLayout(8, 8));
//   gb->contentWidget()->layout()->addWidget(new JZCheckBox(gb->contentWidget()));
class JZGroupBox : public JZWidget
{
    JZ_OBJECT_DECLARE(JZGroupBox)

public:
    explicit JZGroupBox(JZWidget* parent = nullptr);
    explicit JZGroupBox(const std::string& title, JZWidget* parent = nullptr);

    JZSize sizeHint() const override;

    void setTitle(const std::string& t);
    std::string title() const;

    JZWidget* contentWidget() const { return m_content; }

    // 同普通控件: 把布局应用到内部内容区 (等价 contentWidget()->setLayout)
    void setLayout(JZLayout* layout);

protected:
    void paintEvent(JZPainterEvent* event) override;
    void resize(int w, int h) override;

private:
    std::string m_title;
    JZWidget*   m_content = nullptr;   // 内部内容容器 (绘制在标题带之下)
    int m_titleH = 18;                // 标题带高度
    int m_pad = 8;                    // 内容区四周内边距
};

#endif // JZ_GROUP_BOX_H_
