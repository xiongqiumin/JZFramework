#ifndef JZ_CODE_AGENT_MAINWINDOW_H_
#define JZ_CODE_AGENT_MAINWINDOW_H_

#include "JZWindow.h"
#include "JZDialog.h"
#include "JZKeyEvent.h"
#include "JZTextEdit.h"
#include "JZCodeAgent.h"
#include "ChatView.h"

#include "JZLineEdit.h"
#include "JZPushButton.h"
#include "JZLabel.h"
#include "JZTimer.h"
#include "JZMenuBar.h"
#include "JZMenu.h"
#include "JZFileDialog.h"

#include <string>
#include <vector>

// 应用配置（持久化到 exe 目录下的 code_agent.json）
struct AppConfig
{
    std::string modelUrl;
    std::string modelName;
    std::string modelToken;
    std::string workspace;
    // 授信目录：编译器/SDK 等安装目录，虚拟终端可只读访问、可在这里找可执行程序。
    // 不填则只能访问工作区，且没有可用的外部程序。每个目录可单独配置是否可写（默认只读）。
    std::vector<TrustedDir> trustedDirs;
    // 显式指定的可信程序（全路径），如 [{cmake, C:/.../cmake.exe}, {python, C:/.../python.exe}]。
    // 启动时会按本机默认补全缺失项（在 PATH 中探测 cmake/python/git 等并写入全路径）。
    std::vector<TrustedProgram> trustedPrograms;

    // 网络搜索：搜索接口模板(含 {q} 占位符)，空则用内置默认(DuckDuckGo 即时答案)。
    std::string webSearchUrl;
    // HTTP 代理（仅对 http 目标生效；https 目标直连）。host 为空表示不启用。
    std::string proxyHost;
    int proxyPort = 0;
    std::string proxyUser;
    std::string proxyPassword;
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
    void onBrowseWorkspace();
    void onAccept();

    JZLineEdit* m_urlEdit = nullptr;
    JZLineEdit* m_nameEdit = nullptr;
    JZLineEdit* m_tokenEdit = nullptr;
    JZLineEdit* m_wsEdit = nullptr;
    JZLineEdit* m_trustedEdit = nullptr;
    JZPushButton* m_wsBrowseBtn = nullptr;
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
    void onOpenDirectory();
    void onPoll();

    void appendLog(const CodeAgentLog& log);

    JZCodeAgent*  m_agent = nullptr;
    ChatView*     m_chatView = nullptr;
    InputEdit*    m_inputEdit = nullptr;
    JZPushButton* m_sendBtn = nullptr;
    JZPushButton* m_stopBtn = nullptr;
    JZPushButton* m_clearBtn = nullptr;
    JZPushButton* m_settingsBtn = nullptr;
    JZMenuBar*    m_menuBar = nullptr;
    JZLabel*      m_statusLabel = nullptr;
    JZTimer*      m_pollTimer = nullptr;

    int  m_renderedId = 0;
    bool m_busy = false;

    AppConfig m_cfg;
};

#endif // !JZ_CODE_AGENT_MAINWINDOW_H_
