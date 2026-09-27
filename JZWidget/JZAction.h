#ifndef JZ_ACTION_H_
#define JZ_ACTION_H_
#include "JZObject.h"
#include "JZImage.h"
#include "JZSignal.h"
#include <string>

// 菜单项 (菜单栏入口 / 右键菜单条目 / 工具栏按钮的数据模型)。
// 对应 Qt 的 QAction: 持有文本/图标/快捷键/勾选状态, 触发时发 triggered 信号。
// 不直接参与绘制 —— 由 JZMenu / JZMenuBar 负责渲染。
class JZFRAMEWORK_API JZAction : public JZObject
{
    JZ_OBJECT_DECLARE(JZAction)

public:
    explicit JZAction(JZObject* parent = nullptr);
    JZAction(const std::string& text, JZObject* parent = nullptr);

    void setText(const std::string& text);
    std::string text() const;

    void setIcon(const JZImage& icon);
    const JZImage& icon() const;

    void setShortcut(const std::string& shortcut);
    std::string shortcut() const;

    void setEnabled(bool enabled);
    bool isEnabled() const;

    void setCheckable(bool checkable);
    bool isCheckable() const;

    void setChecked(bool checked);
    bool isChecked() const;

    void setSeparator(bool separator);
    bool isSeparator() const;

    // 可见性 (对应 QAction::setVisible): 菜单的布局/绘制/命中/键盘巡航都跳过不可见项 ——
    // 项仍留在 m_actions 里(下标不移动), 只是在菜单里占 0 高度。
    void setVisible(bool visible);
    bool isVisible() const;

    // 关联子菜单 (由 JZMenu::addMenu 设置); 非空表示该项是"子菜单入口"
    void setSubMenu(class JZMenu* menu);
    class JZMenu* subMenu() const;

    // 触发: 翻转 checkable 状态, 再发 triggered 信号
    void trigger();

jz_signals:
    JZSigVoid triggered;

private:
    std::string m_text;
    JZImage m_icon;
    std::string m_shortcut;
    bool m_enabled = true;
    bool m_checkable = false;
    bool m_checked = false;
    bool m_separator = false;
    bool m_visible = true;
    class JZMenu* m_submenu = nullptr;
};

#endif // JZ_ACTION_H_
