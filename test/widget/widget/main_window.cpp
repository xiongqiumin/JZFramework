#include "main_window.h"
#include "JZDialog.h"
#include "JZMessageBox.h"
#include "JZDebug.h"
#include "JZLayout.h"
#include "JZLineEdit.h"

// 主窗口尺寸 (原 lv_obj 百分比由固定像素近似)
static constexpr int kW = 800;
static constexpr int kH = 600;

// 文本对齐 (原 LV_TEXT_ALIGN_*)
enum { JZAlignLeft = 0, JZAlignCenter = 1, JZAlignRight = 2 };

MainWindow::MainWindow(JZWidget* parent)
    : JZWindow(parent)
{
    m_names = {
        "Label", "PushButton", "Slider", "ProgressBar", "Switch", "Checkbox",
        "RadioButton", "ComboBox", "LineEdit", "TextEdit", "Table", "ImageLabel", "Dialog",
        "TabWidget"
    };

    setupUi();
}

MainWindow::~MainWindow()
{
}

void MainWindow::setupUi()
{
    // 中间容器: 左列表 + 右预览
    m_top = new JZWidget(this);

    m_list = new JZListWidget(m_top);
    m_list->addItems(m_names);
    m_list->resize(150, kH);

    m_preview = new JZStackedWidget(m_top);
    m_preview->resize(kW, kH);

    JZHBoxLayout* topLayout = new JZHBoxLayout(4, 4);
    topLayout->addWidget(m_list, 0);    // 固定宽
    topLayout->addWidget(m_preview, 1); // 占剩余宽度
    m_top->setLayout(topLayout);

    // 底部日志
    m_log = new JZTextEdit(this);
    m_log->resize(kW, 120);

    // 主布局: 上(列表+预览) + 下(日志)
    JZVBoxLayout* mainLayout = new JZVBoxLayout(4, 4);
    mainLayout->addWidget(m_top, 1);    // 占主要空间
    mainLayout->addWidget(m_log, 0);    // 固定高
    setLayout(mainLayout);

    buildPages();
    m_list->setCurrentIndex(0);
    connect(&m_list->currentIndexChanged, this, &MainWindow::onSelectChanged);

    log("ready. select a widget on the left.");
}

JZWidget* MainWindow::makePage()
{
    JZWidget* page = new JZWidget(m_preview);
    page->setLayout(new JZVBoxLayout(10, 10));
    return page;
}

void MainWindow::log(const std::string& line)
{
    m_logText += line + "\n";
    if (m_logText.size() > 800)
        m_logText = m_logText.substr(m_logText.size() - 800);
    m_log->setText(m_logText);
}

void MainWindow::onSelectChanged(int idx)
{
    if (idx < 0 || idx >= (int)m_names.size()) return;
    m_preview->setCurrentIndex(idx);
    log("select: " + m_names[idx]);
}

void MainWindow::onLogVoid()        { log("event: clicked"); }
void MainWindow::onLogInt(int v)    { log("value: " + std::to_string(v)); }
void MainWindow::onLogBool(bool b)  { log("state: " + std::string(b ? "on" : "off")); }
void MainWindow::onLogString(const std::string& s) { log("text: " + s); }

void MainWindow::onPbInc()
{
    m_pbVal = (m_pbVal + 10) % 110;
    if (m_pb) m_pb->setValue(m_pbVal, true);
    log("progress: " + std::to_string(m_pbVal));
}

void MainWindow::onLogTextEdit()
{
    if (m_te)
        log("edit: " + m_te->text());
}

void MainWindow::onLabelUpdate()
{
    static int n = 0;
    n++;
    if (m_labelDemo)
        m_labelDemo->setText("updated " + std::to_string(n));
    log("label updated: " + std::to_string(n));
}

void MainWindow::buildPages()
{
    // ---- Label ----
    {
        JZWidget* page = makePage();
        JZLabel* l = new JZLabel(page);
        l->setAlign(JZAlignCenter);
        l->setText("Hello");
        m_labelDemo = l;
        JZPushButton* b = new JZPushButton(page, 100, 32, "Update");
        connect(&b->sigClicked, this, &MainWindow::onLabelUpdate);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(l);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(b);
        m_preview->addWidget(page);
    }

    // ---- PushButton ----
    {
        JZWidget* page = makePage();
        JZPushButton* b = new JZPushButton(page, 120, 44, "Click Me");
        connect(&b->sigClicked, this, &MainWindow::onLogVoid);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(b);
        m_preview->addWidget(page);
    }

    // ---- Slider ----
    {
        JZWidget* page = makePage();
        JZSlider* s = new JZSlider(page);
        s->setRange(0, 100);
        s->setValue(50);
        s->resize(150, s->height());
        connect(&s->sigValueChanged, this, &MainWindow::onLogInt);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(s);
        m_preview->addWidget(page);
    }

    // ---- ProgressBar ----
    {
        JZWidget* page = makePage();
        JZProgressBar* p = new JZProgressBar(page);
        m_pb = p;
        m_pbVal = 60;
        p->setRange(0, 100);
        p->setValue(m_pbVal, true);
        p->resize(150, p->height());
        JZPushButton* b = new JZPushButton(page, 90, 28, "+10");
        connect(&b->sigClicked, this, &MainWindow::onPbInc);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(p);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(b);
        m_preview->addWidget(page);
    }

    // ---- Switch ----
    {
        JZWidget* page = makePage();
        JZSwitch* sw = new JZSwitch(page);
        connect(&sw->sigToggled, this, &MainWindow::onLogBool);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(sw);
        m_preview->addWidget(page);
    }

    // ---- Checkbox ----
    {
        JZWidget* page = makePage();
        JZCheckbox* c = new JZCheckbox(page);
        c->setText("Enable");
        connect(&c->sigToggled, this, &MainWindow::onLogBool);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(c);
        m_preview->addWidget(page);
    }

    // ---- RadioButton ----
    {
        JZWidget* page = makePage();
        JZRadioButton* r1 = new JZRadioButton(page, "Option A");
        JZRadioButton* r2 = new JZRadioButton(page, "Option B");
        connect(&r1->sigToggled, this, &MainWindow::onLogVoid);
        connect(&r2->sigToggled, this, &MainWindow::onLogVoid);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(r1);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(r2);
        m_preview->addWidget(page);
    }

    // ---- ComboBox ----
    {
        JZWidget* page = makePage();
        JZComboBox* cb = new JZComboBox(page);
        cb->addItems({ "Red", "Green", "Blue" });
        connect(&cb->currentIndexChanged, this, &MainWindow::onLogInt);
        connect(&cb->currentTextChanged, this, &MainWindow::onLogString);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(cb);
        m_preview->addWidget(page);
    }
    
    // ---- LineEdit ----
    {
        JZWidget* page = makePage();
        JZLineEdit* line_edit = new JZLineEdit(page);
        line_edit->setText("edit me");
        line_edit->resize(160, 70);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(line_edit);
        m_preview->addWidget(page);
    }
    // ---- TextEdit ----
    {
        JZWidget* page = makePage();
        JZTextEdit* te = new JZTextEdit(page);
        m_te = te;
        te->setText("edit me");
        te->resize(160, 70);
        JZPushButton* b = new JZPushButton(page, 90, 28, "Log");
        connect(&b->sigClicked, this, &MainWindow::onLogTextEdit);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(te);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(b);
        m_preview->addWidget(page);
    }

    // ---- Table ----
    {
        JZWidget* page = makePage();
        JZTableWidget* tbl = new JZTableWidget(page);
        tbl->setColumnCount(3);
        tbl->setRowCount(3);
        tbl->setHorizontalHeaderLabels({ "Id", "Name", "Val" });
        tbl->setCellValue(0, 0, "1");
        tbl->setCellValue(0, 1, "Alpha");
        tbl->setCellValue(0, 2, "99");
        tbl->setCellValue(1, 0, "2");
        tbl->setCellValue(1, 1, "Beta");
        tbl->setCellValue(1, 2, "88");
        tbl->setCellValue(2, 0, "3");
        tbl->setCellValue(2, 1, "Gamma");
        tbl->setCellValue(2, 2, "77");
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(tbl);
        m_preview->addWidget(page);
    }

    // ---- ImageLabel ----
    {
        JZWidget* page = makePage();
        JZImageLabel* img = new JZImageLabel(page);
        img->resize(60, 60);
        JZLabel* note = new JZLabel(page);
        note->setText("(no image loaded)");
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(img);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(note);
        m_preview->addWidget(page);
    }

    // ---- Dialog ----
    {
        JZWidget* page = makePage();
        JZPushButton* b = new JZPushButton(page, 140, 36, "Open Dialog");
        connect(&b->sigClicked, this, &MainWindow::onDialogTest);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(b);
        m_preview->addWidget(page);
    }

    // ---- TabWidget ----
    {
        JZWidget* page = makePage();
        JZTabWidget* tabs = new JZTabWidget(page);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(tabs, 1);

        JZWidget* tabA = new JZWidget();
        tabA->setLayout(new JZVBoxLayout(10, 10));
        JZLabel* la = new JZLabel(tabA);
        la->setText("Tab A content");
        la->setAlign(JZAlignCenter);
        static_cast<JZVBoxLayout*>(tabA->layout())->addWidget(la);

        JZWidget* tabB = new JZWidget();
        tabB->setLayout(new JZVBoxLayout(10, 10));
        JZPushButton* bb = new JZPushButton(tabB, 120, 32, "Btn in Tab B");
        connect(&bb->sigClicked, this, &MainWindow::onLogVoid);
        static_cast<JZVBoxLayout*>(tabB->layout())->addWidget(bb);

        JZWidget* tabC = new JZWidget();
        tabC->setLayout(new JZVBoxLayout(10, 10));
        JZCheckbox* cc = new JZCheckbox(tabC);
        cc->setText("Checkbox in Tab C");
        connect(&cc->sigToggled, this, &MainWindow::onLogBool);
        static_cast<JZVBoxLayout*>(tabC->layout())->addWidget(cc);

        tabs->addTab(tabA, "A");
        tabs->addTab(tabB, "B");
        tabs->addTab(tabC, "C");
        connect(&tabs->currentChanged, this, &MainWindow::onLogInt);
        m_preview->addWidget(page);
    }

    m_preview->setCurrentIndex(0);
}

void MainWindow::onDialogTest()
{
    int r = JZMessageBox::question(this, "Question", "Dialog smoke: continue?")->exec();
    log(std::string("dialog finished: ") + (r == JZDialog::Accepted ? "Accepted" : "Rejected"));
}
