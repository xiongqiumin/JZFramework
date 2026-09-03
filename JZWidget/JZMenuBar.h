#ifndef JZ_MENU_BAR_H_
#define JZ_MENU_BAR_H_

#include "JZWidget.h"
#include <vector>
#include <string>

class JZMenu;

// 菜单栏: 水平排列若干 JZMenu 入口, 点击在对应入口下方展开下拉菜单。
// 菜单数据由 addMenu 创建的 JZMenu 持有, 菜单栏自身只负责标题绘制与弹层触发。
class JZMenuBar : public JZWidget
{
    JZ_OBJECT_DECLARE(JZMenuBar)

public:
    explicit JZMenuBar(JZWidget* parent = nullptr);
    ~JZMenuBar();

    JZSize sizeHint() const override;

    // 添加一个菜单, 返回其 JZMenu 指针 (继续 addAction / addMenu)
    JZMenu* addMenu(const std::string& title);
    int menuCount() const;
    JZMenu* menuAt(int index) const;

    // ---- AI 语义 ----
    std::string aiRole() const override { return "menubar"; }
    std::vector<std::string> aiItems() const override;

protected:
    void paintEvent(JZPainterEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;
    void mouseMoveEvent(JZMouseEvent* event) override;

private:
    std::vector<JZRect> titleRects() const;
    void openMenu(int index);

    std::vector<JZMenu*> m_menus;
    int m_hoverIndex = -1;
    int m_openIndex = -1;
};

#endif // JZ_MENU_BAR_H_
