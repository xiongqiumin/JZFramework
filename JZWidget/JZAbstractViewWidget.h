#ifndef JZ_ABSTRACT_VIEW_WIDGET_H_
#define JZ_ABSTRACT_VIEW_WIDGET_H_
#include "JZScrollArea.h"

// 抽象视图基类: 虚拟绘制 (owner-draw) 视图控件的公共底座。
//
// List/Table/Tree 等"内容很大但只用绘制可视项"的控件都继承本类:
//   - 内容仍在控件自己的 paintEvent 中按 viewportWidth()/viewportHeight() 绘制,
//     滚动位置用 scrollX()/scrollY();
//   - 内容总尺寸由子类重算后调用 setContentSize() 上报, 驱动真实的 JZScrollBar;
//   - 视口尺寸变化 (resize) 时基类统一回调 updateContentSize() 并刷新。
class JZFRAMEWORK_API JZAbstractViewWidget : public JZScrollArea
{
    JZ_OBJECT_DECLARE(JZAbstractViewWidget)

public:
    explicit JZAbstractViewWidget(JZWidget* parent = nullptr);

    // 把虚拟内容坐标 [top, bottom] 所在行滚入视口 (键盘导航/选中保持可见)
    void ensureVisible(int top, int bottom);

protected:
    // 重算内容总尺寸: 数据/布局变化后子类自行调 setContentSize() 上报;
    // 视口尺寸变化时由基类 resizeEvent 回调。
    virtual void updateContentSize() = 0;

    void resizeEvent(JZResizeEvent* event) override;
};

#endif // JZ_ABSTRACT_VIEW_WIDGET_H_
