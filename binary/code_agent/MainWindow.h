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
#include "JZStackedWidget.h"
#include "JZListWidget.h"

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

    // 最近打开的工程（绝对路径，最近使用的排在最前，最多 kMaxRecentProjects 条）。
    // 启动时自动恢复第 0 条，即"默认打开上次的项目"。
    std::vector<std::string> recentProjects;
};

// 最近工程列表最大长度
enum { kMaxRecentProjects = 10 };

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

// 未打开工程时中央区显示的欢迎页：提示先新建/打开工程，并列出最近工程（点击直接打开）。
// 有工程时中央区切到 ChatView，同时隐藏底部输入框与"清空"按钮（无工作区时发指令没有意义）。
class WelcomePage : public JZWidget
{
    JZ_OBJECT_DECLARE(WelcomePage)
public:
    explicit WelcomePage(JZWidget* parent = nullptr);

    // 刷新最近工程列表；列表为空时整块（标题 + 列表）隐藏，只留提示与两个按钮。
    void setRecentProjects(const std::vector<std::string>& paths);

    JZSigVoid   sigNewProject;
    JZSigVoid   sigOpenProject;
    JZSigString sigOpenRecent;      // 参数为工程绝对路径

private:
    void onRecentActivated(int index);

    JZLabel*      m_recentLabel = nullptr;
    JZListWidget* m_recentList = nullptr;
    std::vector<std::string> m_paths;
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

protected:
    // LLM 任务进行中时先弹确认（X 关闭与 文件→退出 都汇聚到 close() 的这一步）。
    void closeEvent(JZCloseEvent* event) override;

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
    void onOpenRecentProject(const std::string& path);   // 欢迎页/最近列表里点开
    void onCloseProject();
    void onExport();
    void onExit();
    void onPoll();
    void onClearRecent();     // 清空"最近的项目"列表

    // 打开 dir 作为工程：探测 → （必要时询问并初始化）→ openProject → 记入最近列表。
    // interactive=false 用于启动自动恢复：不弹任何对话框，非工程目录/失败直接放弃。
    bool openProjectDir(const std::string& dir, bool interactive);
    // 启动时恢复上次的工程（最近列表第 0 条，没有则用配置的工作区）
    void openLastProject();

    // 记入最近工程（同路径上移，去重，截断到 kMaxRecentProjects）
    void addRecentProject(const std::string& path);
    // 重建"最近的项目"子菜单（挂在 aboutToShow 上，展开时才刷新）
    void updateRecentMenu();
    bool checkAgentRun();

    // openProject 成功后的收尾：持久化工作区 + 刷新状态栏 + 整屏重建对话视图
    void finishOpenWorkspace();
    // 按 m_projectOpened 切换中央区（欢迎页 / 对话页），并同步输入框与"清空"的可见性
    void updateStack();

    void appendLog(const CodeAgentLog& log);

    // 右侧对话索引: 刷新列表（每个对话标题为 user 第一条消息的前 30 字符）
    void updateConvList();
    // 切换对话索引: index 为 m_convList 中的条目
    void onSelectConversation(int index);

    // ---- 成员变量 ----

    JZCodeAgent*     m_agent = nullptr;
    ChatView*        m_chatView = nullptr;
    InputEdit*       m_inputEdit = nullptr;
    JZPushButton*    m_clearBtn = nullptr;
    JZMenuBar*       m_menuBar = nullptr;
    JZMenu*          m_recentMenu = nullptr;   // 文件 → 最近的项目（子菜单）
    JZStackedWidget* m_stack = nullptr;        // 0=欢迎页 1=对话页
    WelcomePage*     m_welcome = nullptr;
    bool             m_projectOpened = false;  // 是否已打开工程（决定显示哪一页）

    // ---- 右侧历史面板 ----
    JZWidget*        m_histPanel = nullptr;      // 历史列表面板
    JZListWidget*    m_histList = nullptr;       // 会话历史列表

    // 右键菜单
    JZMenu*          m_histMenu = nullptr;

    std::string m_exportName;   // 上次导出指定的运行时 exe 名（记住，方便连续导出）
    std::string m_exportIcon;   // 上次导出指定的图标路径
    bool m_exportDebug = true;  // 上次导出是否勾选 debug（记住，方便连续导出）
    JZTimer*      m_pollTimer = nullptr;

    std::deque<std::string> m_pendingQueue;

    int  m_renderedId = 0;
    AppConfig m_cfg;
};

#endif // !JZ_CODE_AGENT_MAINWINDOW_H_
