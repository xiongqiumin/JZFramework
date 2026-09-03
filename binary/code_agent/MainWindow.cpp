#include "MainWindow.h"

#include <fstream>
#include "json/json.h"

#include "JZLayout.h"
#include "JZLineEdit.h"
#include "JZPushButton.h"
#include "JZLabel.h"
#include "JZTimer.h"
#include "JZPlatform.h"

using namespace std;

// ==================== InputEdit ====================
InputEdit::InputEdit(JZWidget* parent)
    : JZTextEdit(parent)
    , sigSend(this)
{
}

void InputEdit::keyPressEvent(JZKeyEvent* event)
{
    // VK_RETURN(13) + Ctrl -> 发送
    if (event->key() == 13 && event->ctrl())
    {
        sigSend.emit();
        return;
    }
    JZTextEdit::keyPressEvent(event);
}

// ==================== SettingsDialog ====================
SettingsDialog::SettingsDialog(JZWidget* parent)
    : JZDialog(parent)
{
    setTitle("设置");
    resize(460, 250);
    setLayout(new JZVBoxLayout(8, 8));

    auto addRow = [this](const string& label, JZLineEdit*& edit)
    {
        JZWidget* row = new JZWidget(this);
        JZHBoxLayout* h = new JZHBoxLayout(6, 6);
        JZLabel* l = new JZLabel(row, label);
        l->setFixedWidth(80);
        edit = new JZLineEdit(row);
        h->addWidget(l, 0);
        h->addWidget(edit, 1);
        row->setLayout(h);
        static_cast<JZVBoxLayout*>(this->layout())->addWidget(row, 0);
    };
    addRow("模型地址", m_urlEdit);
    addRow("模型名称", m_nameEdit);
    addRow("API Token", m_tokenEdit);
    addRow("工作区", m_wsEdit);

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
}

void SettingsDialog::getValues(AppConfig& cfg)
{
    cfg.modelUrl = m_urlEdit->text();
    cfg.modelName = m_nameEdit->text();
    cfg.modelToken = m_tokenEdit->text();
    cfg.workspace = m_wsEdit->text();
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
}

void MainWindow::setupUi()
{
    m_statusLabel = new JZLabel(this);
    m_statusLabel->setText("未配置");

    m_outputEdit = new JZTextEdit(this);
    m_outputEdit->setReadOnly(true);
    m_outputEdit->setLineWrap(true);

    // 输入区容器: 左侧多行输入框 + 右侧按钮列。
    // 注意: 放进某个容器 layout 的控件, 其父必须就是该容器,
    // 否则布局用容器坐标系定位, 而控件按父(MainWindow)坐标摆放, 会错位重叠。
    JZWidget* inputArea = new JZWidget(this);
    JZHBoxLayout* inputLayout = new JZHBoxLayout(6, 6);

    m_inputEdit = new InputEdit(inputArea);
    m_inputEdit->setLineWrap(true);
    m_inputEdit->setMinimumHeight(80);
    m_inputEdit->setPlaceholderText("输入任务，回车换行；Ctrl+Enter 或点[发送]执行");
    inputLayout->addWidget(m_inputEdit, 1);

    JZWidget* btnCol = new JZWidget(inputArea);
    JZVBoxLayout* btnColLayout = new JZVBoxLayout(6, 6);
    m_sendBtn = new JZPushButton(btnCol, 90, 30, "发送");
    m_stopBtn = new JZPushButton(btnCol, 90, 30, "停止");
    m_clearBtn = new JZPushButton(btnCol, 90, 30, "清空");
    m_settingsBtn = new JZPushButton(btnCol, 90, 30, "设置");
    btnColLayout->addWidget(m_sendBtn, 0);
    btnColLayout->addWidget(m_stopBtn, 0);
    btnColLayout->addWidget(m_clearBtn, 0);
    btnColLayout->addWidget(m_settingsBtn, 0);
    btnCol->setLayout(btnColLayout);

    inputLayout->addWidget(btnCol, 0);
    inputArea->setLayout(inputLayout);

    JZVBoxLayout* mainLayout = new JZVBoxLayout(6, 6);
    mainLayout->addWidget(m_statusLabel, 0);
    mainLayout->addWidget(m_outputEdit, 1);
    mainLayout->addWidget(inputArea, 0);
    setLayout(mainLayout);

    connect(&m_sendBtn->sigClicked, this, &MainWindow::onSend);
    connect(&m_stopBtn->sigClicked, this, &MainWindow::onStop);
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
    m_cfg.buildCommand.clear();
    m_cfg.runCommand.clear();

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
    if (root.isMember("buildCommand")) m_cfg.buildCommand = root["buildCommand"].asString();
    if (root.isMember("runCommand"))   m_cfg.runCommand = root["runCommand"].asString();

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
    root["buildCommand"] = m_cfg.buildCommand;
    root["runCommand"] = m_cfg.runCommand;

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
    code.buildCommand = m_cfg.buildCommand;
    code.runCommand = m_cfg.runCommand;

    m_agent->init(llm, code);

    m_outputText.clear();
    m_renderedId = 0;
    m_openAssistant = false;
    const auto& logs = m_agent->logs();
    for (size_t i = 0; i < logs.size(); i++)
        appendLog(logs[i]);
    m_renderedId = m_agent->logSeq();
    m_outputEdit->setText(m_outputText);

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
    {
        m_openAssistant = false;
        m_outputText += "你: " + log.text + "\n";
    }
    else if (log.type == "delta")
    {
        if (!m_openAssistant)
        {
            m_outputText += "\n助手: ";
            m_openAssistant = true;
        }
        m_outputText += log.text;
    }
    else if (log.type == "tool_call")
    {
        m_openAssistant = false;
        m_outputText += "\n[工具调用] " + (log.name.empty() ? "" : log.name + " ") + log.text + "\n";
    }
    else if (log.type == "tool_result")
    {
        m_openAssistant = false;
        m_outputText += log.text + "\n";
    }
    else if (log.type == "error")
    {
        m_openAssistant = false;
        m_outputText += "\n[错误] " + log.text + "\n";
    }
    else if (log.type == "info")
    {
        m_openAssistant = false;
        m_outputText += log.text + "\n";
    }
}

void MainWindow::onSend()
{
    if (m_busy)
        return;

    string text = m_inputEdit->text();
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.pop_back();
    if (text.empty())
        return;

    m_agent->request(text);
    m_inputEdit->setText("");
    m_busy = true;
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
    m_outputText.clear();
    m_renderedId = 0;
    m_openAssistant = false;
    const auto& logs = m_agent->logs();
    for (size_t i = 0; i < logs.size(); i++)
        appendLog(logs[i]);
    m_renderedId = m_agent->logSeq();
    m_outputEdit->setText(m_outputText);
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

void MainWindow::onPoll()
{
    int seq = m_agent->logSeq();
    if (seq > m_renderedId)
    {
        const auto& logs = m_agent->logs();
        for (int i = m_renderedId; i < seq; i++)
            appendLog(logs[i]);
        m_renderedId = seq;
        m_outputEdit->setText(m_outputText);
    }

    bool busy = m_agent->isBusy();
    if (busy != m_busy)
    {
        m_busy = busy;
        if (!busy)
        {
            m_openAssistant = false;
            m_outputText += "\n";
            m_outputEdit->setText(m_outputText);
        }
    }
}

JZ_OBJECT_IMPL(InputEdit, JZTextEdit)
JZ_OBJECT_IMPL(SettingsDialog, JZDialog)
JZ_OBJECT_IMPL(MainWindow, JZWindow)
