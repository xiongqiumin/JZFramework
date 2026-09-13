#include "SettingsDialog.h"

#include "JZLabel.h"
#include "JZLayout.h"
#include "JZFileDialog.h"
#include "JZString.h"

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
    m_navList->appendItem("模型配置");
    m_navList->appendItem("路径配置");
    m_navList->appendItem("安全配置");
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

JZ_OBJECT_IMPL(SettingsDialog, JZDialog)
