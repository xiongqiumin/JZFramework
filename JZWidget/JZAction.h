#ifndef JZ_ACTION_H_
#define JZ_ACTION_H_

#include "JZObject.h"
#include "JZImage.h"
#include "JZSignal.h"
#include <string>

// 菜单项 (菜单栏入口 / 右键菜单条目 / 工具栏按钮的数据模型)。
// 对应 Qt 的 QAction: 持有文本/图标/快捷键/勾选状态, 触发时发 triggered 信号。
// 不直接参与绘制 —— 由 JZMenu / JZMenuBar 负责渲染。
class JZAction : public JZObject
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
    class JZMenu* m_submenu = nullptr;
};

#endif // JZ_ACTION_H_
