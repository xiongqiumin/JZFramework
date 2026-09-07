#include "MainWindow.h"

#include <fstream>
#include "json/json.h"

#include "JZLayout.h"
#include "JZLineEdit.h"
#include "JZPushButton.h"
#include "JZLabel.h"
#include "JZTimer.h"
#include "JZPlatform.h"
#include "JZString.h"
#include "JZStd.h"
#include "JZFileInfo.h"
#include "JZMessageBox.h"
#include "JZAction.h"
#include "JZApplication.h"

using namespace std;

// 授信目录输入：多个用分号分隔；每个目录可用 "|rw" 标记为可写（默认只读）。
// 例如: "C:\Python312; C:\MyProj |rw"
static string joinDirList(const vector<TrustedDir>& dirs)
{
    string text;
    for (const auto& d : dirs)
        text += (text.empty() ? string() : string("; ")) + d.path + (d.writable ? " |rw" : "");
    return text;
}

static vector<TrustedDir> parseDirList(const string& text)
{
    vector<TrustedDir> out;
    for (const auto& p : str_split(text, ";", JZ::SkipEmptyParts))
    {
        string s = str_trimmed(p);
        if (s.empty())
            continue;
        TrustedDir td;
        // 末尾 "|rw" / "|ro" 标记读写权限（Windows 路径不含 '|'）
        size_t bar = s.find('|');
        if (bar != string::npos)
        {
            string flag = str_toLower(str_trimmed(s.substr(bar + 1)));
            td.writable = (flag == "rw");
            s = str_trimmed(s.substr(0, bar));
        }
        if (!s.empty())
        {
            td.path = s;
            out.push_back(td);
        }
    }
    return out;
}

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
{
    // 发送按钮嵌入输入框内部: 浮动在底部文本区下方的"按钮带"里。
    // 给文本区留出底部 margin (按钮高度 + 上下间距), 文字永远不与按钮重叠。
    const int btnW = 70, btnH = 30;
    m_sendBtn = new JZPushButton(this, btnW, btnH, "发送");

    // selfLayout 锚定右下角: 右/下锚点下正 dx/dy 向内取反 = 距右/底边距 8/6,
    // 输入框尺寸变化时容器自动 relayout, 无需手写 resizeEvent。
    auto* layout = new JZSelfLayout();
    layout->addWidget(m_sendBtn, JZSelfLayout::BottomRight, 8, 6);
    setLayout(layout);

    connect(&m_sendBtn->sigClicked, this, &InputEdit::onSendClicked);

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

void InputEdit::onSendClicked()
{
    sigSend.emit();
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

// ==================== SettingsDialog ====================
SettingsDialog::SettingsDialog(JZWidget* parent)
    : JZDialog(parent)
{
    setTitle("设置");
    setLayout(new JZVBoxLayout(0, 0));

    // ---- 主区域: 左导航 + 右内容 ----
    JZWidget* mainArea = new JZWidget(this);
    JZHBoxLayout* mainLayout = new JZHBoxLayout(0, 0);

    // 左侧导航列表
    m_navList = new JZListWidget(mainArea);
    m_navList->addItem("模型配置");
    m_navList->addItem("路径配置");
    m_navList->addItem("安全配置");
    m_navList->setFixedWidth(120);
    m_navList->setCurrentIndex(0);
    mainLayout->addWidget(m_navList, 0);

    // 右侧内容区
    m_stack = new JZStackedWidget(mainArea);

    // --- 模型配置页 ---
    {
        JZWidget* page = new JZWidget(m_stack);
        JZVBoxLayout* v = new JZVBoxLayout(12, 10);

        auto addRow = [page, &v](const string& label, JZLineEdit*& edit) {
            JZWidget* row = new JZWidget(page);
            JZHBoxLayout* h = new JZHBoxLayout(6, 6);
            JZLabel* l = new JZLabel(label, row);
            l->setFixedWidth(80);
            edit = new JZLineEdit(row);
            h->addWidget(l, 0);
            h->addWidget(edit, 1);
            row->setLayout(h);
            v->addWidget(row, 0);
        };
        addRow("模型地址", m_urlEdit);
        addRow("模型名称", m_nameEdit);
        addRow("API Token", m_tokenEdit);
        v->addStretch(1);
        page->setLayout(v);
        m_stack->addWidget(page);
    }

    // --- 路径配置页 ---
    {
        JZWidget* page = new JZWidget(m_stack);
        JZVBoxLayout* v = new JZVBoxLayout(12, 10);

        auto addRow = [page, &v](const string& label, JZLineEdit*& edit, bool withBrowse,
                                 JZPushButton*& browseBtn)
        {
            JZWidget* row = new JZWidget(page);
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
            v->addWidget(row, 0);
        };
        addRow("工作区", m_wsEdit, true, m_wsBrowseBtn);
        addRow("TS 环境", m_tsEdit, true, m_tsBrowseBtn);
        m_tsEdit->setPlaceholderText("JS/TS 编译器目录（含 runtime/node 与 tsc），默认 E:/libs/ts_compiler");
        v->addStretch(1);
        page->setLayout(v);
        m_stack->addWidget(page);
    }

    // --- 安全配置页 ---
    {
        JZWidget* page = new JZWidget(m_stack);
        JZVBoxLayout* v = new JZVBoxLayout(12, 10);

        JZLabel* desc = new JZLabel("授信目录中的可执行文件将被允许运行。", page);
        v->addWidget(desc, 0);

        JZWidget* row = new JZWidget(page);
        JZVBoxLayout* rowLayout = new JZVBoxLayout(0, 0);
        m_trustedEdit = new JZLineEdit(row);
        m_trustedEdit->setPlaceholderText("多个用分号分隔，如 C:\\Python312; C:\\MyProj |rw（默认只读，|rw 标记可写）");
        rowLayout->addWidget(m_trustedEdit, 0);
        rowLayout->addStretch(1);
        row->setLayout(rowLayout);
        v->addWidget(row, 1);
        page->setLayout(v);
        m_stack->addWidget(page);
    }

    m_stack->setCurrentIndex(0);
    mainLayout->addWidget(m_stack, 1);
    mainArea->setLayout(mainLayout);
    static_cast<JZVBoxLayout*>(this->layout())->addWidget(mainArea, 1);

    // ---- 底部按钮行 ----
    JZWidget* btnRow = new JZWidget(this);
    JZHBoxLayout* bh = new JZHBoxLayout(12, 8);
    bh->addStretch(1);
    JZPushButton* ok = new JZPushButton(btnRow, 90, 32, "保存");
    JZPushButton* cancel = new JZPushButton(btnRow, 90, 32, "取消");
    bh->addWidget(ok, 0);
    bh->addWidget(cancel, 0);
    btnRow->setLayout(bh);
    static_cast<JZVBoxLayout*>(this->layout())->addWidget(btnRow, 0);

    // ---- 信号 ----
    connect(&m_navList->currentIndexChanged, this, &SettingsDialog::onNavChanged);
    connect(&m_wsBrowseBtn->sigClicked, this, &SettingsDialog::onBrowseWorkspace);
    connect(&m_tsBrowseBtn->sigClicked, this, &SettingsDialog::onBrowseTsEnv);
    connect(&ok->sigClicked, this, &SettingsDialog::onAccept);
    connect(&cancel->sigClicked, this, &SettingsDialog::reject);

    resize(480, 320);
}

void SettingsDialog::onNavChanged(int index)
{
    m_stack->setCurrentIndex(index);
}

void SettingsDialog::setValues(const AppConfig& cfg)
{
    m_urlEdit->setText(cfg.modelUrl);
    m_nameEdit->setText(cfg.modelName);
    m_tokenEdit->setText(cfg.modelToken);
    m_wsEdit->setText(cfg.workspace);
    m_tsEdit->setText(cfg.tsEnvDir);
    m_trustedEdit->setText(joinDirList(cfg.trustedDirs));
}

void SettingsDialog::getValues(AppConfig& cfg)
{
    cfg.modelUrl = m_urlEdit->text();
    cfg.modelName = m_nameEdit->text();
    cfg.modelToken = m_tokenEdit->text();
    cfg.workspace = m_wsEdit->text();
    cfg.tsEnvDir = str_trimmed(m_tsEdit->text());
    cfg.trustedDirs = parseDirList(m_trustedEdit->text());
}

void SettingsDialog::onBrowseWorkspace()
{
    string dir = JZFileDialog::getExistingDirectory(this, "选择工作区目录", m_wsEdit->text());
    if (!dir.empty())
        m_wsEdit->setText(dir);
}

void SettingsDialog::onBrowseTsEnv()
{
    string dir = JZFileDialog::getExistingDirectory(this, "选择 TS 环境目录", m_tsEdit->text());
    if (!dir.empty())
        m_tsEdit->setText(dir);
}

void SettingsDialog::onAccept()
{
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
    resize(900, 640);
    setTitle("JZCodeAgent");

    loadConfig();
    m_agent = new JZCodeAgent(this);
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
    // 退出前给当前工作区存档（agent 是子对象，此时仍然有效）
    if (m_agent)
        m_agent->saveWorkspaceHistory();
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

    // 中央区: 工具条 + 聊天视图 + 底部输入框, 整体作为中央控件挂载。
    // 注意: JZLayout::addWidget 不会自动 reparent, 放进容器布局的子控件父必须就是该容器。
    JZWidget* central = new JZWidget(this);

    // ChatView 上方的工具条: 左侧状态信息, 右侧 [清空]（未打开工程时隐藏）
    JZWidget* topBar = new JZWidget(central);
    topBar->setFixedHeight(32);
    JZHBoxLayout* topLayout = new JZHBoxLayout(6, 0);
    topLayout->addWidget(new JZLabel("CODEBUDDY"));
    topLayout->addStretch();
    m_clearBtn = new JZPushButton(topBar, 84, 28, "清空");
    topLayout->addWidget(m_clearBtn, 0);
    topBar->setLayout(topLayout);

    // 中央区用 stacked widget: 0=欢迎页(未打开工程) 1=对话页。
    // 未打开工程时输入框与"清空"也会隐藏（无工作区时发指令没有意义）。
    m_stack = new JZStackedWidget(central);
    m_welcome = new WelcomePage(m_stack);
    m_chatView = new ChatView(m_stack);
    m_stack->addWidget(m_welcome);
    m_stack->addWidget(m_chatView);

    // 底部输入区: 只有多行输入框(固定高度), 发送按钮由 InputEdit 用 selfLayout
    // 锚定在右下角, 输入框尺寸变化时自动重排。
    m_inputEdit = new InputEdit(central);
    m_inputEdit->setLineWrap(true);
    m_inputEdit->setFixedHeight(104);

    JZVBoxLayout* mainLayout = new JZVBoxLayout(6, 6);
    mainLayout->addWidget(topBar, 0);
    mainLayout->addWidget(m_stack, 1);
    mainLayout->addWidget(m_inputEdit, 0);
    central->setLayout(mainLayout);

    setCentralWidget(central);

    connect(&m_inputEdit->sendButton()->sigClicked, this, &MainWindow::onSendButton);
    connect(&m_clearBtn->sigClicked, this, &MainWindow::onClear);
    connect(&m_inputEdit->sigSend, this, &MainWindow::onSend);
    connect(&m_welcome->sigNewProject, this, &MainWindow::onNewProject);
    connect(&m_welcome->sigOpenProject, this, &MainWindow::onOpenProject);
    connect(&m_welcome->sigOpenRecent, this, &MainWindow::onOpenRecentProject);
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
    llm.maxToken = 200000;

    CodeAgentConfig code;
    code.workspace = m_cfg.workspace;
    code.llmConfig = llm;

    // 运行时环境 = 配置的授信目录/可信程序 + TS 环境目录的动态注入。
    // 动态部分只作用于本次组装、不写回 m_cfg，避免改/清 tsEnvDir 后旧路径残留在持久化配置里。
    vector<TrustedDir> dirs = m_cfg.trustedDirs;
    vector<TrustedProgram> progs = m_cfg.trustedPrograms;
    
    code.tscPath = jzApp->applicationDirPath() + "/compiler/tsc.exe";
    code.trustedDirs = dirs;
    code.trustedPrograms = progs;

    TrustedDir jz_types_dir;
    jz_types_dir.path = jzApp->applicationDirPath() + "/jz_types";
    code.trustedDirs.push_back(jz_types_dir);
    
    code.webSearchUrl = m_cfg.webSearchUrl;
    code.proxyHost = m_cfg.proxyHost;
    code.proxyPort = m_cfg.proxyPort;
    code.proxyUser = m_cfg.proxyUser;
    code.proxyPassword = m_cfg.proxyPassword;

    m_agent->init(code);

    m_chatView->clear();
    m_renderedId = 0;
    const auto& logs = m_agent->logs();
    for (size_t i = 0; i < logs.size(); i++)
        appendLog(logs[i]);
    m_renderedId = m_agent->logSeq();
    m_chatView->scrollToBottom();

    updateStatus();
}

void MainWindow::updateStatus()
{
    
}

void MainWindow::appendLog(const CodeAgentLog& log)
{
    if (log.type == "user")
        m_chatView->appendUser(log.text);
    else if (log.type == "thinking")
        m_chatView->appendThinkingDelta(log.text);
    else if (log.type == "delta")
        m_chatView->appendAssistantDelta(log.text);
    else if (log.type == "assistant")
        m_chatView->appendAssistant(log.text);   // 整条（导入历史时重建用）
    else if (log.type == "tool_call")
    {
        std::vector<std::string> tool_info = str_split(log.name, "$$tool_gap$$");
        m_chatView->appendTool(tool_info[0], tool_info[1], log.text);
    }
    else if (log.type == "tool_result")
    {
        m_chatView->appendToolResult(log.name, log.text);
    }
    else if (log.type == "error")
        m_chatView->appendError(log.text);
    else if (log.type == "info")
        m_chatView->appendInfo(log.text);
}

void MainWindow::onSend()
{
    string text = str_simplified(m_inputEdit->text());
    if (text.empty())
        return;

    // busy 时也照常提交: JZCodeAgent 会缓存到队列, 等上一步发完自动继续
    if (!m_agent->isBusy())
    {
        m_agent->request(text);
        m_inputEdit->setText("");
    }
    else
    {
        m_pendingQueue.push_back(text);
    }
}

void MainWindow::onSendButton()
{
    onSend();
}

void MainWindow::onStop()
{
    if (!m_agent->isBusy())
        return;

    m_agent->stop();
    if (m_pendingQueue.size() > 0)
    {
        string req = m_pendingQueue.front();
        m_pendingQueue.pop_front();
        m_agent->request(req);
    }
}

void MainWindow::onClear()
{
    m_agent->stop();
    m_agent->clearHistory();
    m_renderedId = 0;
    m_chatView->clear();
    const auto& logs = m_agent->logs();
    for (size_t i = 0; i < logs.size(); i++)
        appendLog(logs[i]);
    m_renderedId = m_agent->logSeq();
    m_chatView->scrollToBottom();
}

void MainWindow::onSettings()
{
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

    // 归档当前工作区对话（下次打开/启动恢复会自动接上），然后回到欢迎页。
    // 工作区路径本身保留在 m_cfg，退出与下次启动的恢复逻辑不变。
    m_agent->closeProject();
    m_projectOpened = false;
    m_chatView->clear();
    m_chatView->setWait(false);
    m_renderedId = 0;
    saveConfig();
    updateStack();
}

void MainWindow::updateStack()
{
    if (!m_stack)
        return;

    m_stack->setCurrentIndex(m_projectOpened ? 1 : 0);
    m_inputEdit->setVisible(m_projectOpened);
    m_clearBtn->setVisible(m_projectOpened);
    if (!m_projectOpened)
        m_welcome->setRecentProjects(m_cfg.recentProjects);
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

bool MainWindow::checkAgentRun()
{
    std::string text = "任务正在进行中，退出会中断当前任务。\n\n确定要退出吗？";
    if (JZMessageBox::question(this, "退出 JZCodeAgent", text)->exec() != JZDialog::Accepted)
    {
        return false;
    }
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
    std::string root = codeNormalizePath(m_cfg.workspace);
    std::string outDir, error;
    if (m_agent->exportProject(root, outDir, error, name, icon, dbg))
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

    // 换了工作区 = 换一段对话: openProject() 已存好旧对话、导入新对话并生成日志，
    // 这里整屏重建（agent 的日志 id 已归零，m_renderedId 必须同步归零，否则增量拉取会漏）
    m_chatView->clear();
    m_renderedId = 0;
    const auto& logs = m_agent->logs();
    for (size_t i = 0; i < logs.size(); i++)
        appendLog(logs[i]);
    m_renderedId = m_agent->logSeq();
    m_chatView->scrollToBottom();

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

    bool busy = m_agent->isBusy();
    if (busy && m_pendingQueue.size() > 0)
    {
        std::string message = m_pendingQueue.front();
        m_pendingQueue.pop_front();
        m_agent->request(message);
        //m_chatView->endStream();
    }
}

JZ_OBJECT_IMPL(WelcomePage, JZWidget)
JZ_OBJECT_IMPL(InputEdit, JZTextEdit)
JZ_OBJECT_IMPL(NewProjectDialog, JZDialog)
JZ_OBJECT_IMPL(SettingsDialog, JZDialog)
JZ_OBJECT_IMPL(ExportDialog, JZDialog)
JZ_OBJECT_IMPL(MainWindow, JZMainWindow)
