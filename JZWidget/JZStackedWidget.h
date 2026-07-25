#ifndef JZ_STACKED_WIDGET_H_
#define JZ_STACKED_WIDGET_H_

#include "JZWidget.h"
#include <vector>

// 类似 Qt QStackedWidget: 多个 page 叠放, 同一时刻只显示当前 page
class JZStackedWidget : public JZWidget
{
    JZ_OBJECT_DECLARE(JZStackedWidget)

public:
    explicit JZStackedWidget(JZWidget* parent);

    int addWidget(JZWidget* widget);
    void insertWidget(int index, JZWidget* widget);
    void removeWidget(int index);
    JZWidget* widget(int index) const;
    int count() const;
    int indexOf(JZWidget* widget) const;

    int currentIndex() const;
    void setCurrentIndex(int index);
    JZWidget* currentWidget() const;

    // 页面铺满本控件; sizeHint 取所有页面的最大期望尺寸
    void setGeometry(int x, int y, int w, int h) override;
    void setGeometry(const JZRect& r) override;
    JZSize sizeHint() const override;

private:
    void updateVisibility();

    std::vector<JZWidget*> m_pages;
    int m_current = -1;
};

#endif
