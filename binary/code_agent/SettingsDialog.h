#ifndef JZ_CODE_AGENT_SETTINGSDIALOG_H_
#define JZ_CODE_AGENT_SETTINGSDIALOG_H_

#include "JZDialog.h"
#include "JZLineEdit.h"
#include "JZPushButton.h"
#include "JZListWidget.h"
#include "JZStackedWidget.h"

#include "AppConfig.h"

// 设置对话框：左侧导航（模型 / 路径 / 安全）+ 右侧堆叠页。
// 配置模型地址 / 名称 / Token / 工作区 / TS 环境 / 授信目录。
class SettingsDialog : public JZDialog
{
    JZ_OBJECT_DECLARE(SettingsDialog)
public:
    explicit SettingsDialog(JZWidget* parent = nullptr);

    void setValues(const AppConfig& cfg);
    void getValues(AppConfig& cfg);

protected:
    void onNavChanged(int index);
    void onBrowseWorkspace();
    void onBrowseTsEnv();
    void onAccept();

    // 左侧导航 + 右侧内容
    JZListWidget* m_navList = nullptr;
    JZStackedWidget* m_stack = nullptr;

    // 模型配置页
    JZLineEdit* m_urlEdit = nullptr;
    JZLineEdit* m_nameEdit = nullptr;
    JZLineEdit* m_tokenEdit = nullptr;

    // 路径配置页
    JZLineEdit* m_wsEdit = nullptr;
    JZLineEdit* m_tsEdit = nullptr;
    JZPushButton* m_wsBrowseBtn = nullptr;
    JZPushButton* m_tsBrowseBtn = nullptr;

    // 安全配置页
    JZLineEdit* m_trustedEdit = nullptr;
};

#endif // !JZ_CODE_AGENT_SETTINGSDIALOG_H_
