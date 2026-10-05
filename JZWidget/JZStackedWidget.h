#ifndef JZ_STACKED_WIDGET_H_
#define JZ_STACKED_WIDGET_H_
#include "JZWidget.h"
#include <vector>

// 类似 Qt QStackedWidget: 多个 page 叠放, 同一时刻只显示当前 page
class JZFRAMEWORK_API JZStackedWidget : public JZWidget
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
    // 切到指定页(等价 QStackedWidget::setCurrentWidget): w 不是本控件的页时什么都不做(Qt 同款)。
    void setCurrentWidget(JZWidget* w);

    // 页面铺满本控件; sizeHint 取所有页面的最大期望尺寸
    JZSize sizeHint() const override;
    // 尺寸变化统一在这里处理(框架给子类的钩子, 见 JZWidget::resize 的派发)。
    // 不需要 override setGeometry/resize: setGeometry 内部就是 resize + move,
    // 而 resize 会派发本事件 —— 一个钩子覆盖两条路径。
    void resizeEvent(JZResizeEvent* event) override;
    void paintEvent(JZPainterEvent* event) override;

private:
    void updateVisibility();

    std::vector<JZWidget*> m_pages;
    int m_current = -1;
};

#endif
