#ifndef JZ_MENU_H_
#define JZ_MENU_H_

#include "JZObject.h"
#include "JZImage.h"
#include "JZSignal.h"
#include "JZRect.h"
#include "JZSize.h"
#include <vector>
#include <string>
#include <functional>

class JZWindow;
class JZAction;
class JZMenuView;

// 菜单 (容器): 持有若干 JZAction (含分隔符 / 子菜单), 可作为弹层显示
// (菜单栏下拉 / 右键上下文菜单)。自身是数据 + 弹层控制器, 不直接参与主窗口
// 渲染树 —— 弹层由一个 JZWindow(Popup) 承载, 内容由内部 JZMenuView 绘制。
class JZMenu : public JZObject
{
    JZ_OBJECT_DECLARE(JZMenu)

    friend class JZMenuView;

public:
    explicit JZMenu(JZObject* parent = nullptr);
    JZMenu(const std::string& title, JZObject* parent = nullptr);
    ~JZMenu();

    // 标题 / 图标 (菜单栏入口文本 / 子菜单头)
    void setTitle(const std::string& title);
    std::string title() const;
    void setIcon(const JZImage& icon);
    const JZImage& icon() const;

    // ---- 添加项 ----
    JZAction* addAction(const std::string& text);
    JZAction* addAction(const std::string& text, std::function<void()> triggered);
    JZAction* addAction(JZAction* action);
    void addSeparator();
    JZMenu* addMenu(const std::string& title);   // 子菜单 (返回新建的 JZMenu)

    void clear();
    int actionCount() const;
    JZAction* actionAt(int index) const;

    int menuCount() const;          // 子菜单数量 (action 中 subMenu() 非空的项)
    JZMenu* menuAt(int index) const;

    // ---- 作为弹层显示 ----
    // owner:   所属顶层窗口 (用于屏幕坐标换算 / 模态循环归属)
    // anchor:  触发区 (屏幕逻辑坐标, JZRect 语义为 x/y/w/h); 菜单在其下沿展开; 子菜单在其右侧展开
    //          (注意: 不要按 left/top/right/bottom 传, right()/bottom() 是含 -1 的闭区间边)
    // asSubmenu: true 时向右展开 (供子菜单使用)
    void popup(JZWindow* owner, const JZRect& anchor, bool asSubmenu = false);
    void hidePopup();
    bool isPopupOpen() const;

    // ---- 供 JZMenuView 使用的布局 / 命中查询 (友元) ----
    const std::vector<JZAction*>& actions() const { return m_actions; }
    int itemCount() const { return (int)m_itemRects.size(); }
    const JZRect& itemRect(int i) const { return m_itemRects[i]; }
    JZSize contentSize() const { return m_contentSize; }
    int hitTestItem(int y) const;                 // 命中 action 下标或 -1
    void activateAction(int index);               // 触发并关闭整条弹层链

    JZMenu* parentMenu() const { return m_parentMenu; }
    void setParentMenu(JZMenu* m) { m_parentMenu = m; }
    int hoverIndex() const { return m_hover; }
    void setHoverIndex(int i) { m_hover = i; }

    // ---- AI 语义 ----
    std::string aiRole() const { return "menu"; }
    std::vector<std::string> aiItems() const;

jz_signals:
    JZSigVoid aboutToShow;
    JZSigVoid aboutToHide;
    JZSigInt sigActionTriggered;   // (index) 某个菜单项被触发 —— 值级给 JS 用(JS 不持 action 句柄)

protected:
    void updateLayout();   // 计算每项矩形 / 总尺寸 (内容变化后调用)

private:
    // 把 action 的 triggered 接到 sigActionTriggered(索引) 上(见 JZMenu.cpp)
    void connectActionIndex(JZAction* action);

    void ensurePopup();
    void closeAll();       // 关闭自身及所有父菜单弹层

    std::string m_title;
    JZImage m_icon;
    std::vector<JZAction*> m_actions;
    std::vector<JZRect> m_itemRects;   // 与 m_actions 一一对应 (本地坐标)
    JZSize m_contentSize;

    JZWindow* m_popup = nullptr;
    JZMenuView* m_view = nullptr;
    JZMenu* m_parentMenu = nullptr;     // 若为子菜单, 指向父菜单
    int m_hover = -1;
};

#endif // JZ_MENU_H_
