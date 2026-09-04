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

// 按本机默认探测常用构建/脚本工具；名字需与虚拟终端的程序白名单对应。
static vector<TrustedProgram> detectDefaultPrograms()
{
    struct Cand { string name; vector<string> exes; };
    static const Cand cands[] = {
        {"cmake",  {"cmake.exe", "cmake"}},
        {"python", {"python.exe", "python3.exe", "python"}},
        {"git",    {"git.exe", "git"}},
        {"ninja",  {"ninja.exe", "ninja"}},
        {"node",   {"node.exe", "node"}},
        {"dotnet", {"dotnet.exe", "dotnet"}},
        {"go",     {"go.exe", "go"}},
        {"make",   {"make.exe", "make", "mingw32-make.exe"}},
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

// ==================== InputEdit ====================
InputEdit::InputEdit(JZWidget* parent)
    : JZTextEdit(parent)
    , sigSend(this)
{
    // 发送按钮嵌入输入框内部: 浮动在底部文本区下方的"按钮带"里。
    // 给文本区留出底部 margin (按钮高度 + 上下间距), 文字永远不与按钮重叠。
    const int btnW = 70, btnH = 30;
    m_sendBtn = new JZPushButton(this, btnW, btnH, "发送");
    m_sendBtn->move(width() - m_sendBtn->width() - 8, height() - btnH - 6);
    connect(&m_sendBtn->sigClicked, this, &InputEdit::onSendClicked);

    setTextMargins(14, 10, 14, btnH + 14);   // 底部留: 6(下边距) + 8(与文字间距)
}

void InputEdit::keyPressEvent(JZKeyEvent* event)
{
    // VK_RETURN(13) + Ctrl -> 发送
    if (event->key() == 13 && event->ctrl())
    {
        sigSend.emit();
        return;
    }
    // 组字期间 Ctrl+Enter 也要发送 (composing 判定在基类内部)
    // 已上屏内容之外: 转交抽象基类处理 (含多行回车换行)
    JZAbstractTextEdit::keyPressEvent(event);
}

void InputEdit::resizeEvent(JZResizeEvent* event)
{
    JZAbstractTextEdit::resizeEvent(event);
    // 随输入框尺寸变化把按钮钉在左下角
    m_sendBtn->move(width() - m_sendBtn->width() - 8, height() - m_sendBtn->height() - 6);
}

void InputEdit::onSendClicked()
{
    sigSend.emit();
}

// ==================== SettingsDialog ====================
SettingsDialog::SettingsDialog(JZWidget* parent)
    : JZDialog(parent)
{
    setTitle("设置");
    resize(460, 285);
    setLayout(new JZVBoxLayout(8, 8));

    // withBrowse: 该行末尾额外放一个"浏览..."按钮（用于选择工作区目录）
    auto addRow = [this](const string& label, JZLineEdit*& edit, bool withBrowse)
    {
        JZWidget* row = new JZWidget(this);
        JZHBoxLayout* h = new JZHBoxLayout(6, 6);
        JZLabel* l = new JZLabel(row, label);
        l->setFixedWidth(80);
        edit = new JZLineEdit(row);
        h->addWidget(l, 0);
        h->addWidget(edit, 1);
        if (withBrowse)
        {
            m_wsBrowseBtn = new JZPushButton(row, 70, 28, "浏览...");
            h->addWidget(m_wsBrowseBtn, 0);
        }
        row->setLayout(h);
        static_cast<JZVBoxLayout*>(this->layout())->addWidget(row, 0);
    };
    addRow("模型地址", m_urlEdit, false);
    addRow("模型名称", m_nameEdit, false);
    addRow("API Token", m_tokenEdit, false);
    addRow("工作区", m_wsEdit, true);
    addRow("授信目录", m_trustedEdit, false);
    m_trustedEdit->setPlaceholderText("多个用分号分隔，如 C:\\Python312; C:\\MyProj |rw（默认只读，|rw 标记可写）");

    connect(&m_wsBrowseBtn->sigClicked, this, &SettingsDialog::onBrowseWorkspace);

    JZWidget* btnRow = new JZWidget(this);
    JZHBoxLayout* bh = new JZHBoxLayout(6, 6);
    bh->addStretch(1);
    JZPushButton* ok = new JZPushButton(btnRow, 90, 32, "保存");
    JZPushButton* cancel = new JZPushButton(btnRow, 90, 32, "取消");
    bh->addWidget(ok, 0);
    bh->addWidget(cancel, 0);
    btnRow->setLayout(bh);
    static_cast<JZVBoxLayout*>(this->layout())->addWidget(btnRow, 0);

    connect(&ok->sigClicked, this, &SettingsDialog::onAccept);
    connect(&cancel->sigClicked, this, &SettingsDialog::reject);
}

void SettingsDialog::setValues(const AppConfig& cfg)
{
    m_urlEdit->setText(cfg.modelUrl);
    m_nameEdit->setText(cfg.modelName);
    m_tokenEdit->setText(cfg.modelToken);
    m_wsEdit->setText(cfg.workspace);
    m_trustedEdit->setText(joinDirList(cfg.trustedDirs));
}

void SettingsDialog::getValues(AppConfig& cfg)
{
    cfg.modelUrl = m_urlEdit->text();
    cfg.modelName = m_nameEdit->text();
    cfg.modelToken = m_tokenEdit->text();
    cfg.workspace = m_wsEdit->text();
    cfg.trustedDirs = parseDirList(m_trustedEdit->text());
}

void SettingsDialog::onBrowseWorkspace()
{
    string dir = JZFileDialog::getExistingDirectory(this, "选择工作区目录", m_wsEdit->text());
    if (!dir.empty())
        m_wsEdit->setText(dir);
}

void SettingsDialog::onAccept()
{
    accept();
}

// ==================== MainWindow ====================
MainWindow::MainWindow(JZWidget* parent)
    : JZWindow(parent)
{
    resize(900, 640);
    setTitle("JZCodeAgent");

    loadConfig();
    m_agent = new JZCodeAgent(this);
    setupUi();
    applyConfig();

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
    // 菜单栏: 顶部 File 菜单, 目前只提供"打开目录"(切换工作区)
    m_menuBar = new JZMenuBar(this);
    JZMenu* fileMenu = m_menuBar->addMenu("文件");
    fileMenu->addAction("打开目录", [this]() { onOpenDirectory(); });

    m_chatView = new ChatView(this);

    // ChatView 上方的工具条: 左侧状态信息, 右侧 [清空][设置]
    // 注意: JZLayout::addWidget 不会自动 reparent, 放进容器布局的子控件父必须就是该容器。
    JZWidget* topBar = new JZWidget(this);
    topBar->setFixedHeight(32);
    JZHBoxLayout* topLayout = new JZHBoxLayout(6, 0);
    m_statusLabel = new JZLabel(topBar);
    m_statusLabel->setText("未配置");
    topLayout->addWidget(m_statusLabel, 1);
    m_clearBtn = new JZPushButton(topBar, 84, 28, "清空");
    m_settingsBtn = new JZPushButton(topBar, 84, 28, "设置");
    topLayout->addWidget(m_clearBtn, 0);
    topLayout->addWidget(m_settingsBtn, 0);
    topBar->setLayout(topLayout);

    // 底部输入区: 只有多行输入框(固定高度), 发送按钮由 InputEdit 内嵌在左下角,
    // InputEdit::resizeEvent 里随宽度/高度变化自动调整按钮位置。
    m_inputEdit = new InputEdit(this);
    m_inputEdit->setLineWrap(true);
    m_inputEdit->setFixedHeight(104);
    m_inputEdit->setPlaceholderText("输入任务，回车换行；Ctrl+Enter 或点[发送]");

    JZVBoxLayout* mainLayout = new JZVBoxLayout(6, 6);
    mainLayout->addWidget(m_menuBar, 0);
    mainLayout->addWidget(topBar, 0);
    mainLayout->addWidget(m_chatView, 1);
    mainLayout->addWidget(m_inputEdit, 0);
    setLayout(mainLayout);

    connect(&m_inputEdit->sendButton()->sigClicked, this, &MainWindow::onSendButton);
    connect(&m_clearBtn->sigClicked, this, &MainWindow::onClear);
    connect(&m_settingsBtn->sigClicked, this, &MainWindow::onSettings);
    connect(&m_inputEdit->sigSend, this, &MainWindow::onSend);
}

void MainWindow::loadConfig()
{
    m_cfg.modelUrl.clear();
    m_cfg.modelName.clear();
    m_cfg.modelToken.clear();
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
    code.trustedDirs = m_cfg.trustedDirs;
    code.trustedPrograms = m_cfg.trustedPrograms;
    code.webSearchUrl = m_cfg.webSearchUrl;
    code.proxyHost = m_cfg.proxyHost;
    code.proxyPort = m_cfg.proxyPort;
    code.proxyUser = m_cfg.proxyUser;
    code.proxyPassword = m_cfg.proxyPassword;

    m_agent->init(llm, code);

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
    string s = "模型: " + (m_cfg.modelName.empty() ? "(未配置)" : m_cfg.modelName);
    s += "   工作区: " + m_cfg.workspace;
    if (m_cfg.modelUrl.empty())
        s += "   [请在设置中填写模型地址]";
    m_statusLabel->setText(s);
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
        m_chatView->appendTool(log.name, log.text);
    else if (log.type == "tool_result")
        m_chatView->appendToolResult(log.text);
    else if (log.type == "error")
        m_chatView->appendError(log.text);
    else if (log.type == "info")
        m_chatView->appendInfo(log.text);
}

void MainWindow::onSend()
{
    string text = m_inputEdit->text();
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.pop_back();
    if (text.empty())
        return;

    // busy 时也照常提交: JZCodeAgent 会缓存到队列, 等上一步发完自动继续
    m_agent->request(text);
    m_inputEdit->setText("");
    m_busy = m_agent->isBusy();
    m_inputEdit->sendButton()->setText(m_busy ? "停止" : "发送");
}

void MainWindow::onSendButton()
{
    if (m_busy)
        onStop();
    else
        onSend();
}

void MainWindow::onStop()
{
    if (!m_busy)
        return;
    m_agent->stop();
}

void MainWindow::onClear()
{
    if (m_busy)
        return;

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
    if (m_busy)
        return;

    SettingsDialog dlg(this);
    dlg.setValues(m_cfg);
    if (dlg.exec() == JZDialog::Accepted)
    {
        dlg.getValues(m_cfg);
        saveConfig();
        applyConfig();
    }
}

void MainWindow::onOpenDirectory()
{
    if (m_busy)
        return;

    std::string dir = JZFileDialog::getExistingDirectory(this, "选择工作区目录", m_cfg.workspace);
    if (dir.empty())
        return;

    if (!m_agent->setWorkspace(dir))
        return;

    m_cfg.workspace = dir;
    saveConfig();
    updateStatus();

    // 换了工作区 = 换一段对话: setWorkspace() 已存好旧对话、导入新对话并生成日志，
    // 这里整屏重建（agent 的日志 id 已归零，m_renderedId 必须同步归零，否则增量拉取会漏）
    m_chatView->clear();
    m_renderedId = 0;
    const auto& logs = m_agent->logs();
    for (size_t i = 0; i < logs.size(); i++)
        appendLog(logs[i]);
    m_renderedId = m_agent->logSeq();
    m_chatView->scrollToBottom();
}

void MainWindow::onPoll()
{
    int seq = m_agent->logSeq();
    if (seq > m_renderedId)
    {
        const auto& logs = m_agent->logs();
        for (int i = m_renderedId; i < seq; i++)
            appendLog(logs[i]);
        m_renderedId = seq;
    }

    bool busy = m_agent->isBusy();
    if (busy != m_busy)
    {
        m_busy = busy;
        m_inputEdit->sendButton()->setText(busy ? "停止" : "发送");
        if (!busy)
            m_chatView->endStream();
    }

    // 有指令在排队/续发等待回复时, 在回复后面显示"等待回复中..."
    m_chatView->setWait(m_agent->waitingReply());
}

JZ_OBJECT_IMPL(InputEdit, JZTextEdit)
JZ_OBJECT_IMPL(SettingsDialog, JZDialog)
JZ_OBJECT_IMPL(MainWindow, JZWindow)
