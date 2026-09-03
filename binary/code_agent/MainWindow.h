#ifndef JZ_CODE_AGENT_MAINWINDOW_H_
#define JZ_CODE_AGENT_MAINWINDOW_H_

#include "JZWindow.h"
#include "JZTextEdit.h"
#include "JZDialog.h"
#include "JZKeyEvent.h"
#include "JZCodeAgent.h"

#include "JZLineEdit.h"
#include "JZPushButton.h"
#include "JZLabel.h"
#include "JZTimer.h"

#include <string>

// 应用配置（持久化到 exe 目录下的 code_agent.json）
struct AppConfig
{
    std::string modelUrl;
    std::string modelName;
    std::string modelToken;
    std::string workspace;
    std::string buildCommand;
    std::string runCommand;
};

// 多行输入：Ctrl+Enter 发送
class InputEdit : public JZTextEdit
{
    JZ_OBJECT_DECLARE(InputEdit)
public:
    explicit InputEdit(JZWidget* parent = nullptr);

    JZSigVoid sigSend;

protected:
    void keyPressEvent(JZKeyEvent* event) override;
};

// 设置对话框：配置模型地址 / 名称 / Token / 工作区
class SettingsDialog : public JZDialog
{
    JZ_OBJECT_DECLARE(SettingsDialog)
public:
    explicit SettingsDialog(JZWidget* parent = nullptr);

    void setValues(const AppConfig& cfg);
    void getValues(AppConfig& cfg);

protected:
    void onAccept();

    JZLineEdit* m_urlEdit = nullptr;
    JZLineEdit* m_nameEdit = nullptr;
    JZLineEdit* m_tokenEdit = nullptr;
    JZLineEdit* m_wsEdit = nullptr;
};

// 主窗口：上方输出框 + 下方输入框 + 操作按钮，复用 JZCodeAgent 的本地代码助手能力
class MainWindow : public JZWindow
{
    JZ_OBJECT_DECLARE(MainWindow)
public:
    explicit MainWindow(JZWidget* parent = nullptr);
    ~MainWindow();

private:
    void setupUi();
    void loadConfig();
    void saveConfig();
    void applyConfig();
    void updateStatus();

    void onSend();
    void onStop();
    void onClear();
    void onSettings();
    void onPoll();

    void appendLog(const CodeAgentLog& log);

    JZCodeAgent*  m_agent = nullptr;
    JZTextEdit*   m_outputEdit = nullptr;
    InputEdit*    m_inputEdit = nullptr;
    JZPushButton* m_sendBtn = nullptr;
    JZPushButton* m_stopBtn = nullptr;
    JZPushButton* m_clearBtn = nullptr;
    JZPushButton* m_settingsBtn = nullptr;
    JZLabel*      m_statusLabel = nullptr;
    JZTimer*      m_pollTimer = nullptr;

    std::string m_outputText;
    int  m_renderedId = 0;
    bool m_openAssistant = false;
    bool m_busy = false;

    AppConfig m_cfg;
};

#endif // !JZ_CODE_AGENT_MAINWINDOW_H_
