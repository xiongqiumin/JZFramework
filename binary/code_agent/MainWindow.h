#ifndef JZ_CODE_AGENT_MAINWINDOW_H_
#define JZ_CODE_AGENT_MAINWINDOW_H_

#include "JZMainWindow.h"
#include "JZDialog.h"
#include "JZKeyEvent.h"
#include "JZTextEdit.h"
#include "JZCodeAgent.h"
#include "ChatView.h"
#include "AppConfig.h"
#include "SettingsDialog.h"
#include "TaskPanel.h"
#include "TaskHistoryManager.h"
#include "TokenRing.h"

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
#include "JZSplitter.h"

#include <string>
#include <vector>

// 多行输入：Ctrl+Enter 发送；发送按钮用 selfLayout 锚在输入框内部右下角（忙碌时变"停止"）
class InputEdit : public JZTextEdit
{
    JZ_OBJECT_DECLARE(InputEdit)
public:
    explicit InputEdit(JZWidget* parent = nullptr);

    JZSigVoid sigSend;
    JZSigVoid sigStop;     // 忙碌(且输入框为空)时点击按钮 -> 停止
    JZSigVoid sigShot;     // 点击[截图]: 由上层抓屏后把图片回填到输入框(addImage)
    JZPushButton* sendButton() const { return m_sendBtn; }
    TokenRing* tokenRing() const { return m_tokenRing; }

    // 由外部(MainWindow::onPoll)告知是否正在请求；busy 且输入框为空时按钮显示"停止"，否则"发送"
    void setBusy(bool busy);

    // ---- 随消息发送的图片(截图) ----
    // dataUrl 为实际发给模型的图片(data URL); thumb 为输入框内的预览缩略图(等比缩放)。
    void addImage(const std::string& dataUrl, const JZImage& thumb);
    // 取走全部待发送图片并清空预览(发送后调用)
    std::vector<std::string> takeImages();
    void clearImages();

protected:
    void keyPressEvent(JZKeyEvent* event) override;
    void paintEvent(JZPainterEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;

private:
    void onSendClicked();
    void updateButton();   // 依据 m_busy 与当前文本刷新按钮文案
    JZRect imageSlot(int index) const;        // 第 index 张缩略图在输入框内的槽位
    int    imageAt(const JZPoint& pos) const; // 命中的缩略图下标(-1 未命中)

    JZPushButton* m_sendBtn = nullptr;
    JZPushButton* m_shotBtn = nullptr;      // [截图] 按钮
    TokenRing*    m_tokenRing = nullptr;   // 发送按钮左侧的 token 用量圆环
    bool          m_busy = false;

    std::vector<std::string> m_images;     // 待发送图片(data URL)
    std::vector<JZImage>     m_thumbs;     // 预览缩略图(与 m_images 一一对应)
};

// 工具条上工程名左侧的"运行中"指示：start() 后显示并持续旋转的圆弧，stop() 隐藏。
// 旋转由内部定时器自驱动（约 60ms 一帧），外部只需按忙碌状态调 start/stop。
class BusySpinner : public JZWidget
{
    JZ_OBJECT_DECLARE(BusySpinner)
public:
    explicit BusySpinner(JZWidget* parent = nullptr);

    JZSize sizeHint() const override;

    // 开始旋转（重复调用无副作用）：显示控件并启动内部定时器
    void start();
    // 停止旋转并隐藏
    void stop();

protected:
    void paintEvent(JZPainterEvent* event) override;

private:
    void onTick();      // 定时器帧：推进角度 + 重绘

    JZTimer* m_timer = nullptr;
    bool     m_spinning = false;
    int      m_angle = 0;   // 弧线起始角（度，顺时针，0=3 点方向）
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
// 内容区(左侧任务列表 + 工具条/ChatView/底部输入框)整体作为中央控件 setCentralWidget 挂载。
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
    void onStop();
    void onClear();
    void onScreenshot();               // [截图]: 抓主屏并附加到输入框(随下一条消息发送)
    void onNewTask();                  // 任务面板 [新建任务]
    void onSwitchTask(int id);         // 任务面板 点击历史任务
    void onDeleteTask(int id);         // 任务面板 [删除任务]
    void refreshTaskPanel();           // 按 agent 的任务列表重建面板（版本变化时自动调用）
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
    // 任务进行中做别的操作（新建/切换任务）前的中断确认
    bool confirmInterrupt(const std::string& what);

    // openProject / 切换任务 成功后的收尾：整屏重建对话视图（日志 id 归零后必须全量重绘）
    void reloadChat();
    // 把当前对话写入当前任务的存档（切换/退出前调用）
    void saveCurrentTask();
    // 切到 m_historyManager 的当前任务：重置对话 → 载入该任务的存档 → 重建界面
    void loadCurrentTask();
    // openProject 成功后的收尾：持久化工作区 + 刷新状态栏 + 整屏重建对话视图
    void finishOpenWorkspace();
    // 按 m_projectOpened 切换中央区（欢迎页 / 对话页），并同步输入框与"清空"的可见性
    void updateStack();
    // 把任务面板挂进/摘出 splitter（没打开工程时对话区独占整个宽度）
    void updateSplitter();
    // 把输入框挂进/摘出竖向 splitter（没打开工程时对话区独占整个高度）
    void updateEditorSplitter();
    void onSplitterMoved(int pos, int handleIndex);        // 记住拖动后的面板宽度
    void onEditorSplitterMoved(int pos, int handleIndex);  // 记住拖动后的输入区高度

    // 竖向 splitter 的高度分配：输入区保持 m_inputHeight，剩余全给对话区
    void applyEditorSizes();
    void resizeEvent(JZResizeEvent* event) override;

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
    JZLabel*         m_topLabel = nullptr;     // 工具条上的工程名
    BusySpinner*     m_busySpinner = nullptr;  // 工程名左侧的运行指示（busy 转圈，空闲隐藏）
    JZMenuBar*       m_menuBar = nullptr;
    JZMenu*          m_recentMenu = nullptr;   // 文件 → 最近的项目（子菜单）
    JZStackedWidget* m_stack = nullptr;        // 0=欢迎页 1=对话页
    WelcomePage*     m_welcome = nullptr;
    JZSplitter*      m_splitter = nullptr;     // 任务面板 | 对话区，可拖动调整
    JZSplitter*      m_editorSplitter = nullptr;// 对话区 / 输入区，竖向可拖动
    int              m_inputHeight = 104;      // 输入区高度（拖动后记住）
    TaskPanel*       m_taskPanel = nullptr;    // 左侧任务列表（新建任务 / 历史任务）
    int              m_taskPanelWidth = 220;   // 记住用户拖出来的面板宽度
    int              m_taskVersion = -1;       // 已渲染的任务列表版本（变化才重建）
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

    // 忙碌时排队的下一条指令(文本 + 附图), 上一轮结束后自动发出
    struct PendingRequest
    {
        std::string text;
        std::vector<std::string> images;
    };
    std::deque<PendingRequest> m_pendingQueue;

    int  m_renderedId = 0;
    AppConfig m_cfg;

    // 历史任务（会话）管理：任务索引 + 存档文件位置；对话内容由 m_agent 按路径读写
    TaskHistoryManager m_historyManager;
    bool m_agentWasBusy = false;   // 上一帧的忙碌状态（用来检测"一轮刚结束"）
};

#endif // !JZ_CODE_AGENT_MAINWINDOW_H_
