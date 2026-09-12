#include "MainWindow.h"

#include <fstream>
#include "json/json.h"

#include "JZSplitter.h"
#include "JZLayout.h"
#include "JZLineEdit.h"
#include "JZPushButton.h"
#include "JZLabel.h"
#include "JZTimer.h"
#include "JZPlatform.h"
#include "JZString.h"
#include "JZStd.h"
#include "JZFileInfo.h"
#include "JZDir.h"
#include "JZMessageBox.h"
#include "JZAction.h"
#include "JZApplication.h"
#include "JZPainter.h"
#include "JZScreenShot.h"

using namespace std;

// 输入框内的附图缩略图布局: 一行槽位从左往右排, 与底部按钮带同高
static constexpr int kThumbBox    = 30;   // 槽位边长
static constexpr int kThumbStep   = 36;   // 槽位间距(含间隔)
static constexpr int kThumbLeft   = 14;   // 与输入框左右内边距对齐
static constexpr int kThumbBottom = 6;    // 与底部间距(与按钮一致)
static constexpr int kMaxImages   = 8;    // 单条消息最多附加的截图数

// 在 PATH 环境变量里找一个可执行文件，命中返回全路径（已规范化），否则返回空。
// 用于启动时按本机默认补全可信程序（cmake / python 等）。
static string findExeInPath(const string& exeName)
{
    char* env = nullptr;
#ifdef _WIN32
    _dupenv_s(&env, nullptr, "PATH");
    string pathEnv = env ? env : string();
    if (env) free(env);
#else
    const char* e = getenv("PATH");
    string pathEnv = e ? e : string();
#endif
    if (pathEnv.empty())
        return string();

    vector<string> dirs = str_split(pathEnv, ";");
    if (dirs.size() <= 1)
        dirs = str_split(pathEnv, ":");

    for (const auto& d : dirs)
    {
        string dir = str_trimmed(d);
        if (dir.empty())
            continue;
        string full = codeNormalizePath(dir) + "/" + exeName;
        if (JZFileInfo::exists(full))
            return full;
    }
    return string();
}

// 按本机默认探测常用工具；名字需与虚拟终端的程序白名单对应。
// code_agent 只服务 TypeScript/JavaScript：只需 git（tsc / node 由 TS 环境目录提供）。
static vector<TrustedProgram> detectDefaultPrograms()
{
    struct Cand { string name; vector<string> exes; };
    static const Cand cands[] = {
        {"git",    {"git.exe", "git"}},
    };
    vector<TrustedProgram> out;
    for (const auto& c : cands)
    {
        for (const auto& exe : c.exes)
        {
            string p = findExeInPath(exe);
            if (!p.empty())
            {
                TrustedProgram tp;
                tp.name = c.name;
                tp.path = p;
                out.push_back(tp);
                break;
            }
        }
    }
    return out;
}

// 按 name 覆盖或追加可信程序（同名单条覆盖，保证后注入的 TS 环境优先于 PATH 探测结果）
static void addOrReplaceProgram(vector<TrustedProgram>& list, const TrustedProgram& tp)
{
    for (auto& x : list)
    {
        if (str_toLower(x.name) == str_toLower(tp.name))
        {
            x = tp;
            return;
        }
    }
    list.push_back(tp);
}

// 目录去重追加（不比较大小写差异，目录字符串一般来自同一配置）
static void addDirIfMissing(vector<TrustedDir>& dirs, const TrustedDir& dir)
{
    for (const auto& x : dirs)
    {
        if (str_toLower(x.path) == str_toLower(dir.path))
            return;
    }
    dirs.push_back(dir);
}

// 本机默认的 TS 环境目录（JS/TS 编译器所在，与 ts_compiler 仓库布局一致）
static const char* kDefaultTsEnv = "E:/libs/ts_compiler";

// TS 环境目录的已知布局（与 ts_compiler 仓库一致，已实测原生 tsc.exe 可独立运行）：
//   <env>/runtime/node/node.exe
//   <env>/app/node_modules/@typescript/typescript-win32-x64/lib/tsc.exe
// 目录存在时才注入：目录本身作为只读授信目录，node / tsc 按真实位置作为可信程序。
static void appendTsEnv(const string& envDir, vector<TrustedDir>& dirs, vector<TrustedProgram>& progs)
{
    string root = codeNormalizePath(envDir);
    if (root.empty() || !JZFileInfo::exists(root))
        return;

    TrustedDir td;
    td.path = root;
    td.writable = false;   // TS 环境只读挂载
    addDirIfMissing(dirs, td);

    struct Cand { string name; string rel; };
    static const Cand cands[] = {
        {"node", "runtime/node/node.exe"},
        {"tsc",  "app/node_modules/@typescript/typescript-win32-x64/lib/tsc.exe"},
    };
    for (const auto& c : cands)
    {
        string p = codeNormalizePath(root + "/" + c.rel);
        if (JZFileInfo::exists(p))
        {
            TrustedProgram tp;
            tp.name = c.name;
            tp.path = p;
            addOrReplaceProgram(progs, tp);
        }
    }
}

// ==================== InputEdit ====================
InputEdit::InputEdit(JZWidget* parent)
    : JZTextEdit(parent)
    , sigSend(this)
    , sigStop(this)
    , sigShot(this)
{
    // 发送按钮嵌入输入框内部: 浮动在底部文本区下方的"按钮带"里。
    // 给文本区留出底部 margin (按钮高度 + 上下间距), 文字永远不与按钮重叠。
    const int btnW = 70, btnH = 30;
    m_sendBtn = new JZPushButton(this, btnW, btnH, "发送");

    // 发送按钮左侧的 token 用量圆环（26x26，与按钮垂直居中对齐）
    m_tokenRing = new TokenRing(this);

    // 圆环左侧的 [截图] 按钮：抓主屏并附加到下一条消息
    const int shotW = 60;
    m_shotBtn = new JZPushButton(this, shotW, btnH, "截图");
    m_shotBtn->setTooltip("截取屏幕并附加到下一条消息");

    // selfLayout 锚定右下角: 右/下锚点下正 dx/dy 向内取反 = 距右/底边距 8/6,
    // 输入框尺寸变化时容器自动 relayout, 无需手写 resizeEvent。
    // 由右向左依次摆放: 发送(8) | 圆环(8+btnW+8) | 截图(圆环左侧再留 8)
    auto* layout = new JZSelfLayout();
    layout->addWidget(m_sendBtn, JZSelfLayout::BottomRight, 8, 6);
    layout->addWidget(m_tokenRing, JZSelfLayout::BottomRight,
                      8 + btnW + 8, 6 + (btnH - 26) / 2);
    layout->addWidget(m_shotBtn, JZSelfLayout::BottomRight,
                      8 + btnW + 8 + 26 + 8, 6);
    setLayout(layout);

    connect(&m_sendBtn->sigClicked, this, &InputEdit::onSendClicked);
    connect(&m_shotBtn->sigClicked, this, [this]() { sigShot.emit(); });
    connect(&sigTextChanged, this, [this](const std::string&) { updateButton(); });

    setTextMargins(14, 10, 14, btnH + 14);   // 底部留: 6(下边距) + 8(与文字间距)
    setPlaceholderText("输入任务，Ctrl+Enter换行, 回车或点[发送]");
}

void InputEdit::keyPressEvent(JZKeyEvent* event)
{
    // VK_RETURN(13) + Ctrl -> 发送
    if (event->key() == 13 && !event->ctrl())
    {
        sigSend.emit();
        return;
    }
    // 组字期间 Ctrl+Enter 也要发送 (composing 判定在基类内部)
    // 已上屏内容之外: 转交抽象基类处理 (含多行回车换行)
    JZAbstractTextEdit::keyPressEvent(event);
}

void InputEdit::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    updateButton();
}

void InputEdit::updateButton()
{
    // 正在请求且没有待发内容(文字与附图都为空) -> "停止"；其它情况 -> "发送"
    bool empty = text().empty() && m_images.empty();
    m_sendBtn->setText(m_busy && empty ? "停止" : "发送");
}

void InputEdit::onSendClicked()
{
    // 忙碌且没有待发内容时按"停止"处理，否则视为"发送"
    if (m_busy && text().empty() && m_images.empty())
        sigStop.emit();
    else
        sigSend.emit();
}

// ==================== InputEdit: 附图(截图) ====================
JZRect InputEdit::imageSlot(int index) const
{
    return JZRect(kThumbLeft + index * kThumbStep,
                  height() - kThumbBottom - kThumbBox,
                  kThumbBox, kThumbBox);
}

int InputEdit::imageAt(const JZPoint& pos) const
{
    for (int i = 0; i < (int)m_thumbs.size(); i++)
    {
        if (imageSlot(i).contains(pos))
            return i;
    }
    return -1;
}

void InputEdit::addImage(const std::string& dataUrl, const JZImage& thumb)
{
    if (dataUrl.empty() || (int)m_images.size() >= kMaxImages)
        return;
    m_images.push_back(dataUrl);
    m_thumbs.push_back(thumb);
    updateButton();          // 有图时按钮保持"发送"
    update();                // 立即刷新缩略图预览
}

std::vector<std::string> InputEdit::takeImages()
{
    std::vector<std::string> out;
    out.swap(m_images);
    m_thumbs.clear();
    update();
    return out;
}

void InputEdit::clearImages()
{
    m_images.clear();
    m_thumbs.clear();
    update();
}

void InputEdit::paintEvent(JZPainterEvent* event)
{
    JZTextEdit::paintEvent(event);   // 基类: 外框 + 文本 + 光标

    if (m_thumbs.empty())
        return;

    JZPainter p(this);
    JZFont tagFont(10);
    for (int i = 0; i < (int)m_thumbs.size(); i++)
    {
        JZRect slot = imageSlot(i);

        // 槽位底色/边框
        p.setBrush(JZBrush(JZColor(0xFF, 0xFF, 0xFF)));
        p.setPen(JZPen(JZColor(0xC0, 0xC4, 0xCC), 1));
        p.drawRect(slot.x(), slot.y(), slot.width(), slot.height());

        // 缩略图按真实比例居中摆放(已在 addImage 时缩到槽内)
        const JZImage& t = m_thumbs[i];
        if (!t.isNull())
            p.drawImage(t, slot.x() + (slot.width() - t.width()) / 2,
                        slot.y() + (slot.height() - t.height()) / 2,
                        t.width(), t.height());

        // 右上角小叉: 点击缩略图即移除该图
        p.setFont(tagFont);
        p.setPen(JZPen(JZColor(0xC0, 0x2A, 0x2A), 1));
        p.drawText(slot.right() - 8, slot.y() + 1, "x");
    }
}

void InputEdit::mousePressEvent(JZMouseEvent* event)
{
    if (event->button() == JZMouseEvent::LeftButton)
    {
        int idx = imageAt(event->localPos());
        if (idx >= 0)
        {
            // 点在缩略图上 = 移除该附图(不交给文本编辑, 避免误移光标)
            m_images.erase(m_images.begin() + idx);
            m_thumbs.erase(m_thumbs.begin() + idx);
            updateButton();
            update();
            return;
        }
    }
    JZTextEdit::mousePressEvent(event);
}

// ==================== NewProjectDialog ====================
NewProjectDialog::NewProjectDialog(JZWidget* parent)
    : JZDialog(parent)
{
    setTitle("新建工程");
    resize(520, 200);
    setLayout(new JZVBoxLayout(8, 8));

    // 与 SettingsDialog 相同的行式布局：label + lineEdit（可带"浏览..."按钮）
    auto addRow = [this](const string& label, JZLineEdit*& edit, bool withBrowse,
                         JZPushButton*& browseBtn)
    {
        JZWidget* row = new JZWidget(this);
        JZHBoxLayout* h = new JZHBoxLayout(6, 6);
        JZLabel* l = new JZLabel(label, row);
        l->setFixedWidth(80);
        edit = new JZLineEdit(row);
        h->addWidget(l, 0);
        h->addWidget(edit, 1);
        if (withBrowse)
        {
            browseBtn = new JZPushButton(row, 70, 28, "浏览...");
            h->addWidget(browseBtn, 0);
        }
        row->setLayout(h);
        static_cast<JZVBoxLayout*>(this->layout())->addWidget(row, 0);
    };
    addRow("位置", m_baseEdit, true, m_baseBrowseBtn);
    m_baseEdit->setPlaceholderText("工程所在目录（将在这里创建工程子目录）");
    addRow("工程名", m_nameEdit, false, m_baseBrowseBtn);
    m_nameEdit->setPlaceholderText("新工程名，将作为子目录并初始化为 TS 工程");
    connect(&m_baseBrowseBtn->sigClicked, this, &NewProjectDialog::onBrowseBase);

    JZWidget* btnRow = new JZWidget(this);
    JZHBoxLayout* bh = new JZHBoxLayout(6, 6);
    bh->addStretch(1);
    JZPushButton* ok = new JZPushButton(btnRow, 90, 32, "创建");
    JZPushButton* cancel = new JZPushButton(btnRow, 90, 32, "取消");
    bh->addWidget(ok, 0);
    bh->addWidget(cancel, 0);
    btnRow->setLayout(bh);
    static_cast<JZVBoxLayout*>(this->layout())->addWidget(btnRow, 0);

    connect(&ok->sigClicked, this, &NewProjectDialog::onOk);
    connect(&cancel->sigClicked, this, &NewProjectDialog::reject);
}

void NewProjectDialog::setDefaults(const string& baseDir, const string& name)
{
    m_baseEdit->setText(baseDir);
    m_nameEdit->setText(name);
}

string NewProjectDialog::projectPath() const
{
    string base = str_trimmed(m_baseEdit->text());
    string name = str_trimmed(m_nameEdit->text());
    if (base.empty() || name.empty())
        return string();
    if (base.back() != '/' && base.back() != '\\')
        base += "/";
    return base + name;
}

void NewProjectDialog::onBrowseBase()
{
    string dir = JZFileDialog::getExistingDirectory(this, "选择工程位置", m_baseEdit->text());
    if (!dir.empty())
        m_baseEdit->setText(dir);
}

void NewProjectDialog::onOk()
{
    string base = str_trimmed(m_baseEdit->text());
    string name = str_trimmed(m_nameEdit->text());
    if (base.empty())
    {
        JZMessageBox::warning(this, "新建工程", "请先选择工程所在目录")->exec();
        return;
    }
    if (name.empty())
    {
        JZMessageBox::warning(this, "新建工程", "请输入工程名")->exec();
        return;
    }
    // Windows 目录名不允许的字符
    static const char* kBadChars = "/\\:*?\"<>|";
    if (name.find_first_of(kBadChars) != string::npos || name == "." || name == "..")
    {
        JZMessageBox::warning(this, "新建工程",
                              string("工程名不能包含以下字符: ") + kBadChars)->exec();
        return;
    }
    accept();
}

// ==================== ExportDialog ====================
ExportDialog::ExportDialog(JZWidget* parent)
    : JZDialog(parent)
{
    setTitle("导出工程");
    resize(520, 180);
    setLayout(new JZVBoxLayout(8, 8));

    auto addRow = [this](const string& label, JZLineEdit*& edit, bool withBrowse,
                         JZPushButton*& browseBtn)
    {
        JZWidget* row = new JZWidget(this);
        JZHBoxLayout* h = new JZHBoxLayout(6, 6);
        JZLabel* l = new JZLabel(label, row);
        l->setFixedWidth(80);
        edit = new JZLineEdit(row);
        h->addWidget(l, 0);
        h->addWidget(edit, 1);
        if (withBrowse)
        {
            browseBtn = new JZPushButton(row, 70, 28, "浏览...");
            h->addWidget(browseBtn, 0);
        }
        row->setLayout(h);
        static_cast<JZVBoxLayout*>(this->layout())->addWidget(row, 0);
    };
    addRow("程序名", m_nameEdit, false, m_iconBrowseBtn);
    m_nameEdit->setPlaceholderText("运行时 exe 名（不含 .exe），留空=code_runtime");
    addRow("图标(.ico)", m_iconEdit, true, m_iconBrowseBtn);
    m_iconEdit->setPlaceholderText("可选，指定则替换导出 exe 的图标");
    connect(&m_iconBrowseBtn->sigClicked, this, &ExportDialog::onBrowseIcon);

    // 调试勾选行
    {
        JZWidget* dbgRow = new JZWidget(this);
        JZHBoxLayout* dh = new JZHBoxLayout(6, 6);
        JZLabel* dl = new JZLabel("调试", dbgRow);
        dl->setFixedWidth(80);
        m_debugCheck = new JZCheckbox(dbgRow);
        m_debugCheck->setText("导出 debug 版本（运行时默认开启调试器 JZRuntimeDebugger）");
        m_debugCheck->setChecked(true);
        dh->addWidget(dl, 0);
        dh->addWidget(m_debugCheck, 1);
        dbgRow->setLayout(dh);
        static_cast<JZVBoxLayout*>(this->layout())->addWidget(dbgRow, 0);
    }

    JZWidget* btnRow = new JZWidget(this);
    JZHBoxLayout* bh = new JZHBoxLayout(6, 6);
    bh->addStretch(1);
    JZPushButton* ok = new JZPushButton(btnRow, 90, 32, "导出");
    JZPushButton* cancel = new JZPushButton(btnRow, 90, 32, "取消");
    bh->addWidget(ok, 0);
    bh->addWidget(cancel, 0);
    btnRow->setLayout(bh);
    static_cast<JZVBoxLayout*>(this->layout())->addWidget(btnRow, 0);

    connect(&ok->sigClicked, this, &ExportDialog::onOk);
    connect(&cancel->sigClicked, this, &ExportDialog::reject);
}

void ExportDialog::setDefaults(const string& name, const string& icon, bool debug)
{
    m_nameEdit->setText(name);
    m_iconEdit->setText(icon);
    m_debugCheck->setChecked(debug);
}

string ExportDialog::exeName() const
{
    return str_trimmed(m_nameEdit->text());
}

string ExportDialog::iconPath() const
{
    return str_trimmed(m_iconEdit->text());
}

bool ExportDialog::debug() const
{
    return m_debugCheck->isChecked();
}

void ExportDialog::onBrowseIcon()
{
    string path = JZFileDialog::getOpenFileName(this, "选择图标", m_iconEdit->text(),
                                                "图标 (*.ico)");
    if (!path.empty())
        m_iconEdit->setText(path);
}

void ExportDialog::onOk()
{
    string name = str_trimmed(m_nameEdit->text());
    // 程序名只允许常规文件名字符（去掉可能手误带上的 .exe 由 exportProject 兜底）
    static const char* kBadChars = "/\\:*?\"<>|";
    if (!name.empty() && name.find_first_of(kBadChars) != string::npos)
    {
        JZMessageBox::warning(this, "导出工程", string("程序名不能包含以下字符: ") + kBadChars)
            ->exec();
        return;
    }
    // 图标路径非空时必须是存在的 .ico
    string icon = str_trimmed(m_iconEdit->text());
    if (!icon.empty() && !JZFileInfo::exists(icon))
    {
        JZMessageBox::warning(this, "导出工程", "图标文件不存在: " + icon)->exec();
        return;
    }
    accept();
}

// ==================== WelcomePage ====================
WelcomePage::WelcomePage(JZWidget* parent)
    : JZWidget(parent)
    , sigNewProject(this)
    , sigOpenProject(this)
    , sigOpenRecent(this)
{
    JZVBoxLayout* v = new JZVBoxLayout(12, 8);
    setLayout(v);

    v->addStretch(1);

    JZLabel* title = new JZLabel("JZCodeAgent", this);
    title->setAlign(1);
    title->setFixedHeight(30);
    v->addWidget(title, 0);

    JZLabel* hint = new JZLabel("还没有打开工程。新建或打开一个工程后即可开始。", this);
    hint->setAlign(1);
    hint->setFixedHeight(24);
    v->addWidget(hint, 0);

    JZWidget* btnRow = new JZWidget(this);
    JZHBoxLayout* bh = new JZHBoxLayout(10, 6);
    JZPushButton* newBtn = new JZPushButton(btnRow, 120, 34, "新建工程");
    JZPushButton* openBtn = new JZPushButton(btnRow, 120, 34, "打开工程");
    bh->addStretch(1);
    bh->addWidget(newBtn, 0);
    bh->addWidget(openBtn, 0);
    bh->addStretch(1);
    btnRow->setLayout(bh);
    v->addWidget(btnRow, 0);

    m_recentLabel = new JZLabel("最近的工程", this);
    m_recentLabel->setFixedHeight(24);
    v->addWidget(m_recentLabel, 0);

    m_recentList = new JZListWidget(this);
    m_recentList->setItemHeight(28);
    v->addWidget(m_recentList, 1);

    v->addStretch(1);

    connect(&newBtn->sigClicked, [this]() { sigNewProject.emit(); });
    connect(&openBtn->sigClicked, [this]() { sigOpenProject.emit(); });
    connect(&m_recentList->sigActivated, this, &WelcomePage::onRecentActivated);
}

void WelcomePage::setRecentProjects(const std::vector<std::string>& paths)
{
    m_paths = paths;
    m_recentList->clear();
    for (const auto& p : paths)
        m_recentList->addItem(p);

    m_recentLabel->setVisible(!paths.empty());
    m_recentList->setVisible(!paths.empty());
}

void WelcomePage::onRecentActivated(int index)
{
    if (index < 0 || index >= (int)m_paths.size())
        return;
    sigOpenRecent.emit(m_paths[index]);
}

// ==================== MainWindow ====================
MainWindow::MainWindow(JZWidget* parent)
    : JZMainWindow(parent)
{
    resize(1120, 680);
    setTitle("JZCodeAgent");

    loadConfig();
    m_agent = new JZCodeAgent(this);

    // 任务数据放应用目录（与 code_agent.json 同级），不写进用户的工程目录。
    // 索引是全局的（跨工程），任务条目自己记 workspace，界面按当前工程过滤。
    m_historyManager.setStorageDir(JZPlatform::applicationDirPath() + "/tasks");

    setupUi();

    applyConfig();
    openLastProject();   // 默认打开上次的工程（失败则保持默认工作区）
    updateStack();       // 没打开则停在欢迎页

    m_pollTimer = new JZTimer(this);
    connect(&m_pollTimer->sigTimeout, this, &MainWindow::onPoll);
    m_pollTimer->start(60);
}

MainWindow::~MainWindow()
{
    // 退出前给当前任务存档（agent 是子对象，此时仍然有效）
    saveCurrentTask();
}

void MainWindow::setupUi()
{
    // 菜单栏: 顶部"文件"菜单提供 新建工程 / 打开工程 / 关闭工程 / 最近的项目 / 导出工程 / 设置 / 退出。
    // addMenuBar 会把它插到主窗口第 0 项并自动 reparent。
    m_menuBar = new JZMenuBar(this);
    JZMenu* fileMenu = m_menuBar->addMenu("文件");
    fileMenu->addAction("新建工程", [this]() { onNewProject(); });
    fileMenu->addAction("打开工程", [this]() { onOpenProject(); });
    fileMenu->addAction("关闭工程", [this]() { onCloseProject(); });
    // 最近的项目: 内容在每次展开时按 m_cfg.recentProjects 重建（挂 aboutToShow）。
    m_recentMenu = fileMenu->addMenu("最近的项目");
    connect(&m_recentMenu->aboutToShow, this, &MainWindow::updateRecentMenu);
    updateRecentMenu();
    fileMenu->addAction("导出工程", [this]() { onExport(); });
    fileMenu->addAction("设置", [this]() { onSettings(); });
    fileMenu->addSeparator();
    fileMenu->addAction("退出", [this]() { onExit(); });
    addMenuBar(m_menuBar);

    // 中央区: JZSplitter 分成左右两块 —— 左(导航栏: 搜索 + 任务列表) | 右(工具条 + 聊天视图 + 底部输入框)。
    // splitter 自行管理子控件几何（不走 m_layout），子控件的父必须就是 splitter。
    JZWidget* central = new JZWidget(this);
    m_splitter = new JZSplitter(JZSplitter::Horizontal, central);
    m_splitter->setHandleWidth(4);

    JZWidget* leftArea = new JZWidget(m_splitter);

    // 对话区 | 输入区 用竖向 JZSplitter 分割（可拖动调整输入框高度）
    m_editorSplitter = new JZSplitter(JZSplitter::Vertical, leftArea);
    m_editorSplitter->setHandleWidth(4);

    // ChatView 上方的工具条: 左侧状态信息, 右侧 [清空]（未打开工程时隐藏）
    JZWidget* topBar = new JZWidget(leftArea);
    topBar->setFixedHeight(32);
    JZHBoxLayout* topLayout = new JZHBoxLayout(6, 0);
    m_busySpinner = new BusySpinner(topBar);    // 工程名左侧的运行指示：busy 转圈，空闲隐藏
    topLayout->addWidget(m_busySpinner, 0);
    m_topLabel = new JZLabel("", topBar);       // 打开工程后显示工程名（任务列表归属）
    topLayout->addWidget(m_topLabel, 0);
    topLayout->addStretch();
    m_clearBtn = new JZPushButton(topBar, 84, 28, "清空");
    topLayout->addWidget(m_clearBtn, 0);
    topBar->setLayout(topLayout);

    // 中央区用 stacked widget: 0=欢迎页(未打开工程) 1=对话页。
    // 未打开工程时输入框与"清空"也会隐藏（无工作区时发指令没有意义）。
    m_stack = new JZStackedWidget(m_editorSplitter);
    m_welcome = new WelcomePage(m_stack);
    m_chatView = new ChatView(m_stack);
    m_stack->addWidget(m_welcome);
    m_stack->addWidget(m_chatView);

    // 底部输入区: 多行输入框, 发送按钮与 token 圆环由 InputEdit 用 selfLayout
    // 锚定在右下角, 输入框尺寸变化时自动重排。
    // 不能 setFixedHeight：splitter 靠 setGeometry 摆放，固定高会被夹住而拖不动；
    // 初始高度由 resizeEvent 里的 setSizes 按 m_inputHeight 分配。
    m_inputEdit = new InputEdit(m_editorSplitter);
    m_inputEdit->setLineWrap(true);
    m_inputEdit->setMinimumHeight(64);

    m_editorSplitter->addWidget(m_stack);
    m_editorSplitter->addWidget(m_inputEdit);

    JZVBoxLayout* mainLayout = new JZVBoxLayout(6, 6);
    mainLayout->addWidget(topBar, 0);
    mainLayout->addWidget(m_editorSplitter, 1);
    leftArea->setLayout(mainLayout);

    // 左侧常驻导航栏: 搜索 + 任务列表(关闭工程也常驻, 任务区切到空态)。
    // 工程相关入口(新建/打开/导出/设置)统一放在顶部"文件"菜单里, 面板内不再重复。
    // 不能 setFixedWidth: splitter 靠 setGeometry 摆放子控件, 固定宽会被夹住而拖不动。
    m_navPanel = new NavPanel(m_splitter);

    m_splitter->addWidget(m_navPanel);   // 左: 导航栏
    m_splitter->addWidget(leftArea);     // 右: 对话区
    // 初始比例用拉伸因子(1:4 ≈ 导航栏 1/5 宽)。注意别在这里 setSizes:
    // 此刻 splitter 宽度还是 0, relayout 会把 sizes 全算成 0, 启动后变成左右各一半;
    // 首次 resizeEvent 时再按 m_navWidth 设一次。
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 4);

    JZHBoxLayout* rootLayout = new JZHBoxLayout(0, 0);
    rootLayout->addWidget(m_splitter, 1);
    central->setLayout(rootLayout);

    setCentralWidget(central);

    // 按钮点击已由 InputEdit::onSendClicked 分流为 sigSend / sigStop，这里只接两条语义信号。
    // （不再直连 sendButton()->sigClicked -> onSendButton，避免与 sigSend 重复触发 onSend）
    connect(&m_clearBtn->sigClicked, this, &MainWindow::onClear);
    connect(&m_inputEdit->sigSend, this, &MainWindow::onSend);
    connect(&m_inputEdit->sigStop, this, &MainWindow::onStop);
    connect(&m_inputEdit->sigShot, this, &MainWindow::onScreenshot);
    connect(&m_welcome->sigNewProject, this, &MainWindow::onNewProject);
    connect(&m_welcome->sigOpenProject, this, &MainWindow::onOpenProject);
    connect(&m_welcome->sigOpenRecent, this, &MainWindow::onOpenRecentProject);
    connect(&m_navPanel->sigNewTask, this, &MainWindow::onNewTask);
    connect(&m_navPanel->sigTaskActivated, this, &MainWindow::onSwitchTask);
    connect(&m_navPanel->sigDeleteTask, this, &MainWindow::onDeleteTask);
    connect(&m_splitter->sigSplitterMoved, this, &MainWindow::onSplitterMoved);
    connect(&m_editorSplitter->sigSplitterMoved, this, &MainWindow::onEditorSplitterMoved);
}

void MainWindow::loadConfig()
{
    m_cfg.modelUrl.clear();
    m_cfg.modelName.clear();
    m_cfg.modelToken.clear();
    m_cfg.tsEnvDir.clear();
    m_cfg.workspace = JZPlatform::applicationDirPath();

    string path = JZPlatform::applicationDirPath() + "/code_agent.json";
    ifstream f(path);
    if (!f.is_open())
        return;

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(f, root))
        return;

    if (root.isMember("modelUrl"))     m_cfg.modelUrl = root["modelUrl"].asString();
    if (root.isMember("modelName"))    m_cfg.modelName = root["modelName"].asString();
    if (root.isMember("modelToken"))   m_cfg.modelToken = root["modelToken"].asString();
    if (root.isMember("workspace"))    m_cfg.workspace = root["workspace"].asString();
    // TS 环境目录：JSON 里显式出现过则原样采用（可为空=禁用）；
    // 首次启动没有该字段时用本机默认编译目录（存在才启用），保证开箱即用。
    if (root.isMember("tsEnvDir"))
        m_cfg.tsEnvDir = root["tsEnvDir"].asString();
    else if (JZFileInfo::exists(kDefaultTsEnv))
        m_cfg.tsEnvDir = kDefaultTsEnv;
    if (root.isMember("trustedDirs"))
    {
        const Json::Value& arr = root["trustedDirs"];
        for (int i = 0; i < (int)arr.size(); i++)
        {
            TrustedDir td;
            if (arr[i].isString())
                td.path = arr[i].asString();           // 兼容旧格式：纯字符串，默认只读
            else
            {
                td.path = arr[i].get("path", "").asString();
                td.writable = arr[i].get("writable", false).asBool();
            }
            if (!td.path.empty())
                m_cfg.trustedDirs.push_back(td);
        }
    }
    if (root.isMember("trustedPrograms"))
    {
        const Json::Value& arr = root["trustedPrograms"];
        for (int i = 0; i < (int)arr.size(); i++)
        {
            TrustedProgram tp;
            tp.name = arr[i].get("name", "").asString();
            tp.path = arr[i].get("path", "").asString();
            if (!tp.name.empty() && !tp.path.empty())
                m_cfg.trustedPrograms.push_back(tp);
        }
    }
    if (root.isMember("recentProjects"))
    {
        const Json::Value& arr = root["recentProjects"];
        for (int i = 0; i < (int)arr.size(); i++)
        {
            string p = arr[i].asString();
            if (!p.empty())
                m_cfg.recentProjects.push_back(p);
        }
    }
    if (root.isMember("webSearchUrl")) m_cfg.webSearchUrl = root["webSearchUrl"].asString();
    if (root.isMember("proxyHost"))    m_cfg.proxyHost = root["proxyHost"].asString();
    if (root.isMember("proxyPort"))    m_cfg.proxyPort = root["proxyPort"].asInt();
    if (root.isMember("proxyUser"))    m_cfg.proxyUser = root["proxyUser"].asString();
    if (root.isMember("proxyPassword")) m_cfg.proxyPassword = root["proxyPassword"].asString();

    // 按本机默认补全缺失的可信程序（在 PATH 中探测 cmake/python/git 等并写入全路径），
    // 已显式配置的同名程序不覆盖。
    vector<TrustedProgram> defaults = detectDefaultPrograms();
    for (const auto& d : defaults)
    {
        bool found = false;
        for (const auto& p : m_cfg.trustedPrograms)
            if (str_toLower(p.name) == str_toLower(d.name)) { found = true; break; }
        if (!found)
            m_cfg.trustedPrograms.push_back(d);
    }

    if (m_cfg.workspace.empty())
        m_cfg.workspace = JZPlatform::applicationDirPath();
}

void MainWindow::saveConfig()
{
    Json::Value root;
    root["modelUrl"] = m_cfg.modelUrl;
    root["modelName"] = m_cfg.modelName;
    root["modelToken"] = m_cfg.modelToken;
    root["workspace"] = m_cfg.workspace;
    root["tsEnvDir"] = m_cfg.tsEnvDir;

    Json::Value trusted(Json::arrayValue);
    for (const auto& d : m_cfg.trustedDirs)
    {
        Json::Value o;
        o["path"] = d.path;
        o["writable"] = d.writable;
        trusted.append(o);
    }
    root["trustedDirs"] = trusted;

    Json::Value progs(Json::arrayValue);
    for (const auto& p : m_cfg.trustedPrograms)
    {
        Json::Value o;
        o["name"] = p.name;
        o["path"] = p.path;
        progs.append(o);
    }
    root["trustedPrograms"] = progs;

    Json::Value recent(Json::arrayValue);
    for (const auto& p : m_cfg.recentProjects)
        recent.append(p);
    root["recentProjects"] = recent;

    root["webSearchUrl"] = m_cfg.webSearchUrl;
    root["proxyHost"] = m_cfg.proxyHost;
    root["proxyPort"] = m_cfg.proxyPort;
    root["proxyUser"] = m_cfg.proxyUser;
    root["proxyPassword"] = m_cfg.proxyPassword;

    Json::StyledWriter writer;
    string path = JZPlatform::applicationDirPath() + "/code_agent.json";
    ofstream f(path);
    if (f.is_open())
        f << writer.write(root);
}

void MainWindow::applyConfig()
{
    JZAgentConfig llm;
    llm.modelUrl = m_cfg.modelUrl;
    llm.modelName = m_cfg.modelName;
    llm.modelToken = m_cfg.modelToken;
    llm.maxToken = 160 * 1000;

    CodeAgentConfig code;
    code.workspace = m_cfg.workspace;
    code.llmConfig = llm;

    // 运行时环境 = 配置的授信目录/可信程序 + TS 环境目录的动态注入。
    // 动态部分只作用于本次组装、不写回 m_cfg，避免改/清 tsEnvDir 后旧路径残留在持久化配置里。
    vector<TrustedDir> dirs = m_cfg.trustedDirs;
    vector<TrustedProgram> progs = m_cfg.trustedPrograms;
    
    // 宿主侧 tsc：与 benchmark / shell 工具的 exeSearchPaths 同源（bin 下只有 ts_compiler/tsc/，
    // 旧路径 compiler/tsc.exe 不存在，会让 buildProject 在前置校验就失败）
    code.tscPath = jzApp->applicationDirPath() + "/ts_compiler/tsc/tsc.exe";
    code.trustedDirs = dirs;
    code.trustedPrograms = progs;

    // 随 exe 部署的类型定义: 新部署在 jz_engine/jz_types(见 package.py), 旧平铺布局兼容保留
    TrustedDir jz_types_dir;
    jz_types_dir.path = jzApp->applicationDirPath() + "/jz_engine/jz_types";
    code.trustedDirs.push_back(jz_types_dir);
    jz_types_dir.path = jzApp->applicationDirPath() + "/jz_types";
    code.trustedDirs.push_back(jz_types_dir);
    
    code.webSearchUrl = m_cfg.webSearchUrl;
    code.proxyHost = m_cfg.proxyHost;
    code.proxyPort = m_cfg.proxyPort;
    code.proxyUser = m_cfg.proxyUser;
    code.proxyPassword = m_cfg.proxyPassword;

    m_agent->init(code);

    // 任务列表跟着工作区走；init() 重建了 agent（对话被清空），把当前任务再载回来
    m_historyManager.setWorkspace(m_agent->codeConfig().workspace);
    m_historyManager.load();
    loadCurrentTask();
    refreshTaskPanel();
    updateStatus();
}

void MainWindow::updateStatus()
{
    if (!m_topLabel)
        return;
    m_topLabel->setText(m_projectOpened && !m_cfg.workspace.empty()
                        ? JZDir(m_cfg.workspace).name() : string());
}

void MainWindow::appendLog(const CodeAgentLog& log)
{
    if (log.type == "user")
        m_chatView->appendUser(log.text, log.images);
    else if (log.type == "thinking")
        m_chatView->appendThinkingDelta(log.text);
    else if (log.type == "delta")
        m_chatView->appendAssistantDelta(log.text);
    else if (log.type == "assistant")
        m_chatView->appendAssistant(log.text);   // 整条（导入历史时重建用）
    else if (log.type == "tool_call")
    {
        std::vector<std::string> tool_info = str_split(log.name, "$$tool_gap$$");
        ToolCall call;
        call.function_name = tool_info[0];
        call.id = tool_info.size() > 1 ? tool_info[1] : "";
        call.function_args = log.text;
        m_chatView->appendTool(call);
    }
    else if (log.type == "tool_result")
    {
        ToolCallResult result;
        result.id = log.name;
        result.result = log.text;
        result.isOk = true;                 // 历史重建时无成败信息, 默认成功
        m_chatView->appendToolResult(result);
    }
    else if (log.type == "error")
        m_chatView->appendError(log.text);
    else if (log.type == "info")
        m_chatView->appendInfo(log.text);
}

void MainWindow::onSend()
{
    string text = str_simplified(m_inputEdit->text());
    vector<string> images = m_inputEdit->takeImages();   // 附图随本条消息一起发出
    if (text.empty() && images.empty())
        return;

    // busy 时也照常提交: JZCodeAgent 会缓存到队列, 等上一步发完自动继续
    if (!m_agent->isBusy())
    {
        // 首条消息定标题 / 计一条(仅截图时用占位标题)
        m_historyManager.touchTask(text.empty() ? string("(截图)") : text);
        m_agent->request(text, images);
        m_inputEdit->setText("");
        refreshTaskPanel();
    }
    else
    {
        PendingRequest req;
        req.text = text;
        req.images = images;
        m_pendingQueue.push_back(req);
    }
}

void MainWindow::onScreenshot()
{
    if (!m_projectOpened || !m_inputEdit)
        return;

    // 全屏截图窗口: 抓整个虚拟桌面 -> 覆盖全屏的标注窗口(矩形/椭圆/画笔)
    // -> 确认后把"截图 + 标注"附加到输入框。取消返回空图, 静默返回。
    JZImage shot = JZScreenShot::captureInteractive(this);
    if (shot.isNull())
        return;

    string dataUrl = JZScreenShot::toDataUrl(shot);
    if (dataUrl.empty())
    {
        JZMessageBox::warning(this, "截图", "截图编码失败")->exec();
        return;
    }

    m_inputEdit->addImage(dataUrl, JZScreenShot::fitTo(shot, kThumbBox - 4, kThumbBox - 4));
    m_inputEdit->setFocus();   // 截完直接回到输入框继续打字
}

void MainWindow::onStop()
{
    if (!m_agent->isBusy())
        return;

    saveCurrentTask();   // 先归档：stop() 会重建 agent 丢掉内存里的对话
    m_agent->stop();
    if (m_pendingQueue.size() > 0)
    {
        PendingRequest req = m_pendingQueue.front();
        m_pendingQueue.pop_front();
        m_agent->request(req.text, req.images);
    }
}

void MainWindow::onClear()
{
    m_agent->stop();
    m_agent->clearHistory();

    // 当前任务的对话一起清掉，否则下次启动/切回来旧对话又会自己出现
    m_historyManager.clearConversation(m_historyManager.currentTaskId());
    m_historyManager.resetMessageCount();
    reloadChat();
    refreshTaskPanel();
}

void MainWindow::saveCurrentTask()
{
    if (!m_agent || !m_projectOpened)
        return;
    int id = m_historyManager.currentTaskId();
    if (id > 0)
        m_historyManager.saveConversation(id, m_agent->conversationJson());
}

void MainWindow::loadCurrentTask()
{
    if (!m_agent)
        return;
    // 调用方需先 resetConversation()；这里只负责把该任务的对话读进来并重建界面
    int id = m_historyManager.currentTaskId();
    if (id > 0)
        m_agent->loadConversationJson(m_historyManager.conversationJson(id));
    reloadChat();
}

void MainWindow::reloadChat()
{
    // 整屏重建：切任务 / 打开工程后 agent 的日志 id 已归零，m_renderedId 必须同步归零，
    // 否则增量拉取会因为 id 倒退而漏掉新日志。
    m_chatView->clear();
    m_chatView->setWait(false);
    m_renderedId = 0;
    const auto& logs = m_agent->logs();
    for (size_t i = 0; i < logs.size(); i++)
        appendLog(logs[i]);
    m_renderedId = m_agent->logSeq();
    m_chatView->scrollToBottom();
}

void MainWindow::refreshTaskPanel()
{
    if (!m_navPanel)
        return;
    m_taskVersion = m_historyManager.taskVersion();
    m_navPanel->setTasks(m_historyManager.tasks(), m_historyManager.currentTaskId());
}

void MainWindow::onNewTask()
{
    if (!m_projectOpened)
        return;
    if (m_agent->isBusy() && !confirmInterrupt("新建任务"))
        return;

    saveCurrentTask();
    m_agent->resetConversation();       // 新任务 = 一段干净的新对话
    m_historyManager.newTask();
    reloadChat();
    refreshTaskPanel();
}

void MainWindow::onSwitchTask(int id)
{
    if (!m_projectOpened || id <= 0)
        return;
    if (id == m_historyManager.currentTaskId())
        return;
    if (m_agent->isBusy() && !confirmInterrupt("切换任务"))
        return;

    saveCurrentTask();
    m_agent->resetConversation();
    if (m_historyManager.switchTask(id))
        loadCurrentTask();
    refreshTaskPanel();
}

void MainWindow::onDeleteTask(int id)
{
    if (!m_projectOpened || id <= 0)
        return;

    string text = "删除后该任务的对话记录也会一并删除，无法恢复。\n\n确定要删除这个任务吗？";
    if (JZMessageBox::question(this, "删除任务", text)->exec() != JZDialog::Accepted)
        return;

    bool wasCurrent = (id == m_historyManager.currentTaskId());
    m_historyManager.deleteTask(id);
    if (wasCurrent)
    {
        // 删掉的是当前任务：manager 已经落到另一条，把它的对话载进来
        m_agent->resetConversation();
        loadCurrentTask();
    }
    refreshTaskPanel();
}

void MainWindow::onSettings()
{
    saveCurrentTask();      // stop() 会重建 agent 丢掉内存里的对话
    m_agent->stop();

    SettingsDialog dlg(this);
    dlg.setValues(m_cfg);
    if (dlg.exec() == JZDialog::Accepted)
    {
        dlg.getValues(m_cfg);
        saveConfig();
        applyConfig();
    }
}

void MainWindow::onOpenProject()
{
    saveCurrentTask();      // stop() 会重建 agent 丢掉内存里的对话
    m_agent->stop();

    std::string dir = JZFileDialog::getExistingDirectory(this, "打开工程", m_cfg.workspace);
    if (dir.empty())
        return;

    openProjectDir(dir, true);
}

bool MainWindow::openProjectDir(const std::string& dir, bool interactive)
{
    // 先探测工程属性：不是 TS 工程目录时，询问是否初始化成标准工程（不打扰已识别工程）
    CodeProjectStatus st = m_agent->probeProject(dir);
    if (!st.pathOk)
    {
        if (interactive)
        {
            JZMessageBox::warning(this, "无法打开工程",
                                  st.error.empty() ? "打开工程失败" : st.error)->exec();
        }
        return false;
    }

    // openProject 会切工作区并重建 agent，先把旧任务的对话归档
    saveCurrentTask();

    std::string error;
    if (!m_agent->openProject(dir, &st, error))
    {
        if (interactive)
            JZMessageBox::warning(this, "无法打开工程", error.empty() ? "打开工程失败" : error)->exec();
        return false;
    }

    m_cfg.workspace = codeNormalizePath(dir);
    addRecentProject(m_cfg.workspace);
    m_projectOpened = true;
    finishOpenWorkspace();
    return true;
}

void MainWindow::onOpenRecentProject(const std::string& path)
{
    openProjectDir(path, true);
}

void MainWindow::onCloseProject()
{
    if (!m_projectOpened)
        return;

    // 归档当前任务对话（下次打开/启动恢复会自动接上），然后回到欢迎页。
    // 工作区路径本身保留在 m_cfg，退出与下次启动的恢复逻辑不变。
    saveCurrentTask();
    m_agent->closeProject();
    m_projectOpened = false;
    m_chatView->clear();
    m_chatView->setWait(false);
    m_renderedId = 0;
    saveConfig();
    m_historyManager.setWorkspace(std::string());   // 任务属于工作区：一并清空
    refreshTaskPanel();
    updateStack();
}

void MainWindow::updateStack()
{
    if (!m_stack)
        return;

    m_stack->setCurrentIndex(m_projectOpened ? 1 : 0);
    m_clearBtn->setVisible(m_projectOpened);
    updateSplitter();
    updateEditorSplitter();
    if (!m_projectOpened)
        m_welcome->setRecentProjects(m_cfg.recentProjects);
}

// 导航栏始终常驻; updateSplitter 只把"工程是否打开"传给 NavPanel,
// 由它内部在 TaskPanel / 空态提示 之间切换。
void MainWindow::updateSplitter()
{
    if (m_navPanel)
        m_navPanel->setProjectOpened(m_projectOpened);
}

void MainWindow::onSplitterMoved(int pos, int handleIndex)
{
    std::vector<int> sizes = m_splitter->sizes();
    if (!sizes.empty() && sizes[0] >= 200)
        m_navWidth = sizes[0];
}

// 竖分割：没打开工程时输入框隐藏，欢迎页占满整个高度
void MainWindow::updateEditorSplitter()
{
    if (!m_inputEdit)
        return;

    m_inputEdit->setVisible(m_projectOpened);
    if (m_projectOpened)
        applyEditorSizes();
}

// 输入区保持固定高度，剩余全给对话区（JZSplitter 默认是按比例缩放，这里手动纠正）
void MainWindow::applyEditorSizes()
{
    if (!m_editorSplitter || !m_inputEdit || !m_inputEdit->isVisible())
        return;

    int total = m_editorSplitter->height() - m_editorSplitter->handleWidth();
    if (total <= 0)
        return;

    int h = m_inputHeight;
    int maxH = std::max(0, total - 80);        // 对话区至少留 80
    h = std::min(h, maxH);
    h = std::max(h, std::min(64, total));      // 输入区至少 64（空间不足时按可用高度）
    m_editorSplitter->setSizes({total - h, h});
}

// 首次 resizeEvent 时按 m_navWidth 设置左侧导航栏初始宽度(之后由用户拖动决定, 不再覆盖)
void MainWindow::applyNavWidth()
{
    if (!m_splitter || !m_navPanel)
        return;
    int total = m_splitter->width() - m_splitter->handleWidth();
    if (total <= 0)
        return;
    int w = std::min(m_navWidth, std::max(200, total - 400));
    m_splitter->setSizes({w, total - w});
}

void MainWindow::onEditorSplitterMoved(int pos, int handleIndex)
{
    std::vector<int> sizes = m_editorSplitter->sizes();
    if (sizes.size() >= 2 && sizes[1] >= 64)
        m_inputHeight = sizes[1];
}

void MainWindow::resizeEvent(JZResizeEvent* event)
{
    JZMainWindow::resizeEvent(event);
    // 窗口尺寸变化后重算: 输入区高度跟着 m_inputHeight 走, 不被按比例拉伸
    applyEditorSizes();
    // 首次有尺寸时按 m_navWidth 给左侧导航栏一个稳定的初始宽度,
    // 之后由用户拖动 + onSplitterMoved 决定, 不再覆盖。
    if (!m_navInitialSized && m_splitter && m_splitter->width() > 0)
    {
        applyNavWidth();
        m_navInitialSized = true;
    }
}

void MainWindow::openLastProject()
{
    std::string last;
    if (!m_cfg.recentProjects.empty())
        last = m_cfg.recentProjects.front();
    else if (!m_cfg.workspace.empty())
        last = m_cfg.workspace;

    if (last.empty())
        return;

    openProjectDir(last, false);   // 失败静默：仍然是 init 时的默认工作区
}

void MainWindow::addRecentProject(const std::string& path)
{
    if (path.empty())
        return;

    std::string p = codeNormalizePath(path);
    // 已存在则先移除（大小写不敏感），再插到最前
    for (auto it = m_cfg.recentProjects.begin(); it != m_cfg.recentProjects.end(); )
    {
        if (str_toLower(*it) == str_toLower(p))
            it = m_cfg.recentProjects.erase(it);
        else
            ++it;
    }
    m_cfg.recentProjects.insert(m_cfg.recentProjects.begin(), p);
    if ((int)m_cfg.recentProjects.size() > kMaxRecentProjects)
        m_cfg.recentProjects.resize(kMaxRecentProjects);
}

void MainWindow::updateRecentMenu()
{
    if (!m_recentMenu)
        return;
    m_recentMenu->clear();

    if (m_cfg.recentProjects.empty())
    {
        // popup() 对空菜单直接返回（点了没反应），放一条禁用项占位
        JZAction* none = m_recentMenu->addAction("（无）");
        none->setEnabled(false);
        return;
    }

    for (int i = 0; i < (int)m_cfg.recentProjects.size(); i++)
    {
        std::string path = m_cfg.recentProjects[i];
        std::string text = std::to_string(i + 1) + ". " + path;
        m_recentMenu->addAction(text, [this, path]() { openProjectDir(path, true); });
    }
    m_recentMenu->addSeparator();
    m_recentMenu->addAction("清除列表", [this]() { onClearRecent(); });
}

void MainWindow::onClearRecent()
{
    // activateAction 已先收起弹层再执行本回调, 这里重建菜单也是安全的;
    // 仍交给下一次 aboutToShow 刷新, 保持单一刷新路径。
    m_cfg.recentProjects.clear();
    saveConfig();
}

void MainWindow::onNewProject()
{
    if (!checkAgentRun())
        return;

    NewProjectDialog dlg(this);
    dlg.setDefaults(m_cfg.workspace, "");
    if (dlg.exec() != JZDialog::Accepted)
        return;

    std::string path = dlg.projectPath();
    if (path.empty())
        return;

    // 目标位置策略：已是工程 → 直接打开不重复初始化；空目录 → 直接初始化；
    // 有内容但非工程 → 询问后再初始化（initProject 幂等，不覆盖已有文件）。
    CodeProjectStatus st = m_agent->probeProject(path);
    bool doInit = true;
    if (st.pathOk && st.isProject)
    {
        doInit = false;
    }
    else if (st.pathOk && !st.isEmpty)
    {
        std::string text = "目录已存在且有内容：\n" + path + "\n\n";
        text += "仍要把它初始化成标准 TS 工程吗？\n";
        text += "（生成 tsconfig.json(outDir=build) / src/main.ts / .gitignore，不覆盖已有文件）";
        if (JZMessageBox::question(this, "新建工程", text)->exec() != JZDialog::Accepted)
            return;
    }

    if (doInit)
    {
        std::string error;
        if (!m_agent->initProject(path, error))
        {
            JZMessageBox::warning(this, "初始化工程失败",
                                  error.empty() ? "未知错误" : error)->exec();
            return;
        }
    }

    openProjectDir(path, true);
}

void MainWindow::onExit()
{
    close();   // Qt 习惯: close() -> sendEvent(closeEvent) -> accept 则隐藏并销毁窗口
}

bool MainWindow::confirmInterrupt(const std::string& what)
{
    std::string text = "任务正在进行中，" + what + "会中断当前任务。\n\n确定要继续吗？";
    return JZMessageBox::question(this, "JZCodeAgent", text)->exec() == JZDialog::Accepted;
}

bool MainWindow::checkAgentRun()
{
    std::string text = "任务正在进行中，退出会中断当前任务。\n\n确定要退出吗？";
    if (JZMessageBox::question(this, "退出 JZCodeAgent", text)->exec() != JZDialog::Accepted)
    {
        return false;
    }
    saveCurrentTask();     // 先归档：stop() 会重建 agent 丢掉内存里的对话
    m_agent->stop();       // 确认退出: 中断当前任务再走关闭流程
    return true;
}

// 关闭确认: LLM 任务进行中时先询问要不要中断退出。
// X 关闭 (WM_CLOSE) 与 文件→退出 都汇聚到 close() 的 sendEvent(closeEvent) 这一步,
// 所以两种入口都会走到这里。取消 -> ignore(), close() 不再往下执行, 窗口留在原地。
void MainWindow::closeEvent(JZCloseEvent* event)
{
    if (m_agent && m_agent->isBusy())
    {
        if (!checkAgentRun())
        {
            event->ignore();   // 留在应用里继续跑
            return;
        }
    }
    event->accept();
}

void MainWindow::onExport()
{
    if (!m_projectOpened)
    {
        JZMessageBox::warning(this, "导出工程", "请先新建或打开一个工程")->exec();
        return;
    }

    // 先弹出导出对话框，收集运行时 exe 名称、图标与是否 debug（可留空）。
    ExportDialog dlg(this);
    dlg.setDefaults(m_exportName, m_exportIcon, m_exportDebug);
    if (dlg.exec() != JZDialog::Accepted)
        return;

    std::string name = dlg.exeName();
    std::string icon = dlg.iconPath();
    bool dbg = dlg.debug();

    // 导出当前打开的工程（工作区即工程根）为可独立运行的发行包：
    // exportProject 会重新打包/发布，并把 build/ + project.json + code_runtime.exe（改名/换图标/
    // 写调试开关资源）拷到 <工程>/export。
    ExportConfig cfg;
    cfg.exeName = name;
    cfg.iconPath = icon;
    cfg.debug = dbg;
    std::string outDir, error;
    if (m_agent->exportProject(outDir, error, cfg))
    {
        // 记住本次选择，方便连续导出
        m_exportName = name;
        m_exportIcon = icon;
        m_exportDebug = dbg;

        std::string shown = name.empty() ? "code_runtime" : name;
        std::string msg = "已导出可独立运行包到：\n" + outDir +
                          "\n\n包含 " + shown + ".exe（内嵌 JZEngineRuntime" +
                          (icon.empty() ? "" : "，已替换为自定义图标") +
                          (dbg ? "，已写入调试开关资源（运行时可开启 JZRuntimeDebugger）" : "") +
                          "）+ 编译产物 build/ + project.json，"
                          "双击 run.bat 即可在本机运行。";
        JZMessageBox::information(this, "导出工程", msg)->exec();
    }
    else
    {
        JZMessageBox::warning(this, "导出失败", error.empty() ? "未知错误" : error)->exec();
    }
}

void MainWindow::finishOpenWorkspace()
{
    saveConfig();
    updateStatus();

    // 换了工作区 = 换一批任务：按新工作区重载任务索引，再把当前任务的对话载进来
    m_historyManager.setWorkspace(m_agent->codeConfig().workspace);
    m_historyManager.load();
    loadCurrentTask();
    refreshTaskPanel();
    updateStack();
}

void MainWindow::onPoll()
{
    int seq = m_agent->logSeq();
    if (seq > m_renderedId)
    {
        const auto& logs = m_agent->logs();
        // 不能拿全局序号当 vector 下标：m_agent 会裁剪旧日志(m_logs>3000 从头部删)，
        // logSeq() 返回的是全局单调递增 id，下标与 id 不一致，按 log.id 迭代才安全。
        for (const auto& log : logs)
        {
            if (log.id <= m_renderedId)
                continue;
            if (log.id > seq)
                break;
            appendLog(log);
        }
        m_renderedId = seq;
    }

    // token 用量圆环：当前上下文累计用量 / 上限（悬停提示按 K 折算）
    m_inputEdit->tokenRing()->setUsage(m_agent->usage().total_tokens,
                                       m_agent->llmConfig().maxToken);

    bool busy = m_agent->isBusy();
    m_inputEdit->setBusy(busy);   // 驱动输入框按钮在"发送"/"停止"间切换

    // 工程名左侧的运行指示：运行中转圈，空闲隐藏（控件内部定时器自驱动）
    if (busy)
        m_busySpinner->start();
    else
        m_busySpinner->stop();

    // 一轮刚结束：助手回了话，计一条消息并把对话落盘
    if (m_agentWasBusy && !busy)
    {
        m_historyManager.addMessage();
        saveCurrentTask();
    }
    m_agentWasBusy = busy;

    // 任务列表变了（新任务 / 标题定下来 / 一轮结束）才重建，避免每帧刷新
    if (m_historyManager.taskVersion() != m_taskVersion)
        refreshTaskPanel();

    if (!busy && m_pendingQueue.size() > 0)
    {
        m_chatView->endStream();

        PendingRequest req = m_pendingQueue.front();
        m_pendingQueue.pop_front();
        m_agent->request(req.text, req.images);
    }
}

JZ_OBJECT_IMPL(WelcomePage, JZWidget)
JZ_OBJECT_IMPL(InputEdit, JZTextEdit)
JZ_OBJECT_IMPL(BusySpinner, JZWidget)
JZ_OBJECT_IMPL(NewProjectDialog, JZDialog)
JZ_OBJECT_IMPL(ExportDialog, JZDialog)
JZ_OBJECT_IMPL(MainWindow, JZMainWindow)

// ==================== BusySpinner ====================
BusySpinner::BusySpinner(JZWidget* parent)
    : JZWidget(parent)
{
    setFixedSize(16, 16);
    setVisible(false);   // 初始隐藏，start() 时才显示

    m_timer = new JZTimer(this);
    connect(&m_timer->sigTimeout, this, &BusySpinner::onTick);
}

JZSize BusySpinner::sizeHint() const
{
    return JZSize(16, 16);
}

void BusySpinner::start()
{
    if (m_spinning)
        return;
    m_spinning = true;
    m_angle = 0;
    setVisible(true);
    m_timer->start(60);
}

void BusySpinner::stop()
{
    if (!m_spinning)
        return;
    m_spinning = false;
    m_timer->stop();
    setVisible(false);
}

void BusySpinner::onTick()
{
    m_angle = (m_angle + 30) % 360;   // 30°/帧(60ms) ≈ 0.72s 一圈
    update();
}

void BusySpinner::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);
    JZRect rc = rect();
    int cx = rc.x() + rc.width() / 2;
    int cy = rc.y() + rc.height() / 2;
    int r = (rc.width() < rc.height() ? rc.width() : rc.height()) / 2 - 3;

    p.setPen(JZPen(JZColor(0x2b, 0x90, 0xe0), 2));   // 与 TokenRing 一致的主题蓝
    // 270° 圆弧 = 带缺口的转圈；跨越 0° 时拆成两段画，不依赖渲染层的角度环绕处理
    int start = m_angle % 360;
    const int sweep = 270;
    if (start + sweep <= 360)
        p.drawArc(cx, cy, r, start, start + sweep);
    else
    {
        p.drawArc(cx, cy, r, start, 360);
        p.drawArc(cx, cy, r, 0, start + sweep - 360);
    }
}
