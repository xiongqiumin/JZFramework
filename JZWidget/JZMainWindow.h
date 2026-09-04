#ifndef JZ_MAIN_WINDOW_H_
#define JZ_MAIN_WINDOW_H_

#include "JZWindow.h"

class JZMenuBar;

// 主窗口: QMainWindow 式的"顶部菜单栏 + 中央区"骨架。
//   内部用一个无内边距的 VBox 布局, 菜单栏贴顶占满整行, 中央控件吃掉其余空间;
//   addMenuBar / setCentralWidget 都会自动把控件 reparent 到本窗口,
//   显示次序固定为 菜单栏(可选) -> 中央区(可选)。setCentralWidget 的中央控件
//   需要自带布局负责内部内容排列 (本类只保证它占满剩余区域)。
class JZMainWindow : public JZWindow
{
    JZ_OBJECT_DECLARE(JZMainWindow)

public:
    explicit JZMainWindow(JZWidget* parent = nullptr);

    // 设置顶部菜单栏。重复调用替换旧菜单栏(不删除旧对象); 菜单栏始终排在最顶行。
    void addMenuBar(JZMenuBar* menuBar);
    JZMenuBar* menuBar() const { return m_menuBar; }

    // 设置中央控件 (占用菜单栏外的全部剩余空间)。重复调用替换旧的(不删除旧对象)。
    void setCentralWidget(JZWidget* widget);
    JZWidget* centralWidget() const { return m_centralWidget; }

protected:
    void paintEvent(JZPainterEvent* event) override;

private:
    JZMenuBar* m_menuBar = nullptr;
    JZWidget*  m_centralWidget = nullptr;
};

#endif // JZ_MAIN_WINDOW_H_
