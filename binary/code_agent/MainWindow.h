#ifndef JZ_CODE_AGENT_MAINWINDOW_H_
#define JZ_CODE_AGENT_MAINWINDOW_H_

#include "JZMainWindow.h"
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
#include "JZCheckbox.h"

#include <string>
#include <vector>

// 应用配置（持久化到 exe 目录下的 code_agent.json）
struct AppConfig
{
    std::string modelUrl;
    std::string modelName;
    std::string modelToken;
    std::string workspace;
    // TS 环境目录（JS/TS 编译器所在，默认 E:/libs/ts_compiler）：
    // 应用时自动把该目录作为只读授信目录挂载，并按真实布局注入 node / tsc 可信程序。
    std::string tsEnvDir;
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

// 多行输入：Ctrl+Enter 发送；发送按钮用 selfLayout 锚在输入框内部右下角（忙碌时变"停止"）
class InputEdit : public JZTextEdit
{
    JZ_OBJECT_DECLARE(InputEdit)
public:
    explicit InputEdit(JZWidget* parent = nullptr);

    JZSigVoid sigSend;
    JZPushButton* sendButton() const { return m_sendBtn; }

protected:
    void keyPressEvent(JZKeyEvent* event) override;

private:
    void onSendClicked();

    JZPushButton* m_sendBtn = nullptr;
};

// 新建工程对话框：选工程所在目录（已存在）+ 输入工程名；
// 确认后把 <位置>/<工程名> 作为工程目录，初始化标准 TS 工程骨架并打开。
class NewProjectDialog : public JZDialog
{
    JZ_OBJECT_DECLARE(NewProjectDialog)
public:
    explicit NewProjectDialog(JZWidget* parent = nullptr);

    void setDefaults(const std::string& baseDir, const std::string& name);
    std::string projectPath() const;   // 接受后返回 <位置>/<工程名>

protected:
    void onBrowseBase();
    void onOk();                       // 校验工程名合法后 accept()

    JZLineEdit* m_baseEdit = nullptr;
    JZLineEdit* m_nameEdit = nullptr;
    JZPushButton* m_baseBrowseBtn = nullptr;
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
    void onBrowseTsEnv();
    void onAccept();

    JZLineEdit* m_urlEdit = nullptr;
    JZLineEdit* m_nameEdit = nullptr;
    JZLineEdit* m_tokenEdit = nullptr;
    JZLineEdit* m_wsEdit = nullptr;
    JZLineEdit* m_tsEdit = nullptr;
    JZLineEdit* m_trustedEdit = nullptr;
    JZPushButton* m_wsBrowseBtn = nullptr;
    JZPushButton* m_tsBrowseBtn = nullptr;
};

// 导出对话框：导出工程为可独立运行的发行包时，指定运行时 exe 名称、图标与是否 debug。
//   - 程序名：导出后运行时 exe 的文件名（不含 .exe，留空=code_runtime）。
//   - 图标(.ico)：可选项，指定则把该图标写入导出的运行时 exe（JZExeResource 替换资源图标）。
//   - 调试：勾选则写入"调试开关"资源（缺省勾选），运行时自身读取后开启 JZRuntimeDebugger。
class ExportDialog : public JZDialog
{
    JZ_OBJECT_DECLARE(ExportDialog)
public:
    explicit ExportDialog(JZWidget* parent = nullptr);

    void setDefaults(const std::string& name, const std::string& icon, bool debug);
    std::string exeName() const;     // 接受后返回运行时 exe 名（不含扩展名，可能为空）
    std::string iconPath() const;    // 接受后返回图标路径（可能为空）
    bool debug() const;              // 接受后返回是否导出 debug 版本

protected:
    void onBrowseIcon();
    void onOk();

    JZLineEdit* m_nameEdit = nullptr;
    JZLineEdit* m_iconEdit = nullptr;
    JZPushButton* m_iconBrowseBtn = nullptr;
    JZCheckbox* m_debugCheck = nullptr;
};

// 主窗口：直接继承 JZMainWindow 骨架。菜单栏经 addMenuBar 置顶，
// 内容区(状态/工具条 + ChatView + 底部输入框)整体作为中央控件 setCentralWidget 挂载。
// 复用 JZCodeAgent 的本地代码助手能力
class MainWindow : public JZMainWindow
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
    void onSendButton();      // 发送按钮点击：空闲=发送，忙碌=停止
    void onStop();
    void onClear();
    void onSettings();
    void onNewProject();
    void onOpenProject();
    void onExport();
    void onExit();
    void onPoll();

    // openProject 成功后的收尾：持久化工作区 + 刷新状态栏 + 整屏重建对话视图
    void finishOpenWorkspace();

    void appendLog(const CodeAgentLog& log);

    JZCodeAgent*  m_agent = nullptr;
    ChatView*     m_chatView = nullptr;
    InputEdit*    m_inputEdit = nullptr;
    JZPushButton* m_clearBtn = nullptr;
    JZPushButton* m_settingsBtn = nullptr;
    JZPushButton* m_exportBtn = nullptr;
    JZMenuBar*    m_menuBar = nullptr;

    std::string m_exportName;   // 上次导出指定的运行时 exe 名（记住，方便连续导出）
    std::string m_exportIcon;   // 上次导出指定的图标路径
    bool m_exportDebug = true;  // 上次导出是否勾选 debug（记住，方便连续导出）
    JZLabel*      m_statusLabel = nullptr;
    JZTimer*      m_pollTimer = nullptr;

    int  m_renderedId = 0;
    bool m_busy = false;

    AppConfig m_cfg;
};

#endif // !JZ_CODE_AGENT_MAINWINDOW_H_
