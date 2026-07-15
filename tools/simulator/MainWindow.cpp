#include "MainWindow.h"
#include "JZSimulator.h"

#include <QWidget>
#include <QHBoxLayout>
#include <QString>

MainWindow::MainWindow(JZSimulator *sim, JZSimulatorBridge *bridge, QWidget *parent)
    : QMainWindow(parent), m_sim(sim), m_bridge(bridge)
{
    // 连接模型 -> 视图 的 Qt 信号 (自动排队到 UI 线程, 跨线程安全)
    connect(m_bridge, &JZSimulatorBridge::logAppended, this, &MainWindow::appendLog);
    connect(m_bridge, &JZSimulatorBridge::stateChanged, this, &MainWindow::refreshStatus);
    connect(m_bridge, &JZSimulatorBridge::periphCharsChanged, this, &MainWindow::rebuildPeriphCharUI);

    buildUI();
    switchTab(0);
    refreshStatus();
}

MainWindow::~MainWindow() {}

// ======================== UI 构建 ========================
void MainWindow::buildUI()
{
    setWindowTitle("JZ BLE Simulator");
    resize(780, 700);

    auto *central = new QWidget(this);
    setCentralWidget(central);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    // ---- 上部: 左标签 + 右堆叠 ----
    auto *topArea = new QWidget(central);
    auto *topLayout = new QHBoxLayout(topArea);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(8);

    auto *leftPanel = new QWidget(topArea);
    leftPanel->setFixedWidth(180);
    auto *leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(6);

    auto *tabPeriph = new QPushButton("BLE Peripheral", leftPanel);
    auto *tabCent   = new QPushButton("BLE Central", leftPanel);
    auto *tabConf   = new QPushButton("Config", leftPanel);
    leftLayout->addWidget(tabPeriph);
    leftLayout->addWidget(tabCent);
    leftLayout->addWidget(tabConf);
    leftLayout->addStretch(1);

    m_tabBtns = {tabPeriph, tabCent, tabConf};
    QObject::connect(tabPeriph, &QPushButton::clicked, this, [this]() { switchTab(0); });
    QObject::connect(tabCent,   &QPushButton::clicked, this, [this]() { switchTab(1); });
    QObject::connect(tabConf,   &QPushButton::clicked, this, [this]() { switchTab(2); });

    m_stack = new QStackedWidget(topArea);
    buildPeripheralPage();
    buildCentralPage();
    buildConfigPage();

    topLayout->addWidget(leftPanel);
    topLayout->addWidget(m_stack, 1);

    root->addWidget(topArea, 1);

    // ---- 下部: 状态 + 日志 ----
    auto *bottomArea = new QWidget(central);
    auto *bottomLayout = new QVBoxLayout(bottomArea);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->setSpacing(4);

    auto *statusRow = new QWidget(bottomArea);
    auto *statusLayout = new QHBoxLayout(statusRow);
    statusLayout->setContentsMargins(0, 0, 0, 0);
    statusLayout->setSpacing(4);
    statusLayout->addWidget(new QLabel("Status:", statusRow));
    m_statusLabel = new QLabel("idle", statusRow);
    statusLayout->addWidget(m_statusLabel, 1);

    bottomLayout->addWidget(statusRow);
    bottomLayout->addWidget(new QLabel("Log:", bottomArea));
    m_logEdit = new QTextEdit(bottomArea);
    m_logEdit->setReadOnly(true);
    bottomLayout->addWidget(m_logEdit, 1);

    root->addWidget(bottomArea);
}

void MainWindow::buildPeripheralPage()
{
    auto *page = new QWidget();
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    layout->addWidget(new QLabel("Device as BLE Peripheral"));

    // 设备名
    auto *nameRow = new QWidget(page);
    auto *nameLayout = new QHBoxLayout(nameRow);
    nameLayout->setContentsMargins(0, 0, 0, 0);
    nameLayout->setSpacing(4);
    nameLayout->addWidget(new QLabel("Device Name:"));
    m_nameEdit = new QLineEdit(page);
    m_nameEdit->setText(QString::fromStdString(m_sim->deviceName()));
    nameLayout->addWidget(m_nameEdit);
    auto *applyBtn = new QPushButton("Apply", page);
    nameLayout->addWidget(applyBtn);
    nameLayout->addStretch(1);
    QObject::connect(applyBtn, &QPushButton::clicked, this, [this]() {
        m_sim->setDeviceName(m_nameEdit->text().toStdString());
    });
    layout->addWidget(nameRow);

    // 广播
    auto *advRow = new QWidget(page);
    auto *advLayout = new QHBoxLayout(advRow);
    advLayout->setContentsMargins(0, 0, 0, 0);
    advLayout->setSpacing(4);
    advLayout->addWidget(new QLabel("Advertising:"));
    m_advSwitch = new QCheckBox(page);
    m_advSwitch->setChecked(m_sim->isAdvertising());
    advLayout->addWidget(m_advSwitch);
    advLayout->addStretch(1);
    QObject::connect(m_advSwitch, &QCheckBox::toggled, this, [this](bool v) {
        m_sim->setAdvertising(v);
    });
    layout->addWidget(advRow);

    layout->addWidget(new QLabel("Notify -> App Central  |  App Central can Read/Write these chars"));

    // 特征列表
    m_charContainer = new QWidget(page);
    m_charLayout = new QVBoxLayout(m_charContainer);
    m_charLayout->setContentsMargins(0, 0, 0, 0);
    m_charLayout->setSpacing(4);
    const auto &chars = m_sim->chars();
    for (size_t i = 0; i < chars.size(); i++) {
        auto *row = new QWidget(m_charContainer);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(4);
        rowLayout->addWidget(new QLabel(QString::fromStdString(chars[i].chr.substr(0, 8) + "...")));
        rowLayout->addWidget(new QLabel("Val:"));
        auto *ve = new QLineEdit(m_charContainer);
        ve->setText(QString::fromStdString(chars[i].value));
        ve->setFixedWidth(120);
        rowLayout->addWidget(ve);
        auto *nb = new QPushButton("Notify", m_charContainer);
        rowLayout->addWidget(nb);
        rowLayout->addStretch(1);
        QObject::connect(nb, &QPushButton::clicked, this, [this, i, ve]() {
            m_sim->notifyChar(i, ve->text().toStdString());
        });
    }
    layout->addWidget(m_charContainer);
    layout->addStretch(1);

    m_stack->addWidget(page);
}

void MainWindow::buildCentralPage()
{
    auto *page = new QWidget();
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    layout->addWidget(new QLabel("Device as BLE Central (App Peripheral upstream test)"));

    auto *ctrlRow = new QWidget(page);
    auto *ctrlLayout = new QHBoxLayout(ctrlRow);
    ctrlLayout->setContentsMargins(0, 0, 0, 0);
    ctrlLayout->setSpacing(4);
    auto *connBtn = new QPushButton("Connect to App Peripheral", page);
    auto *discBtn = new QPushButton("Disconnect", page);
    ctrlLayout->addWidget(connBtn);
    ctrlLayout->addWidget(discBtn);
    ctrlLayout->addStretch(1);
    QObject::connect(connBtn, &QPushButton::clicked, this, [this]() { m_sim->connectCentral(); });
    QObject::connect(discBtn, &QPushButton::clicked, this, [this]() { m_sim->disconnectCentral(); });
    layout->addWidget(ctrlRow);

    layout->addWidget(new QLabel("App Peripheral chs appear here, Write tests upstream flow (onBleCharWritten)"));

    m_periphCharContainer = new QWidget(page);
    m_periphCharLayout = new QVBoxLayout(m_periphCharContainer);
    m_periphCharLayout->setContentsMargins(0, 0, 0, 0);
    m_periphCharLayout->setSpacing(4);
    layout->addWidget(m_periphCharContainer);
    layout->addStretch(1);

    m_stack->addWidget(page);
    rebuildPeriphCharUI();
}

void MainWindow::buildConfigPage()
{
    auto *page = new QWidget();
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    layout->addWidget(new QLabel("Config"));
    layout->addWidget(new QLabel("Default GATT chars are pre-configured in JZSimulator constructor.\n"
                                 "Edit JZSimulator::JZSimulator() to change services/characteristics."));
    layout->addStretch(1);

    m_stack->addWidget(page);
}

void MainWindow::switchTab(int idx)
{
    if (idx < 0 || idx > 2) return;
    m_stack->setCurrentIndex(idx);
    for (size_t i = 0; i < m_tabBtns.size(); i++)
        m_tabBtns[i]->setEnabled(i != (size_t)idx);
}

// ======================== 模型 -> 视图 刷新 ========================
void MainWindow::refreshStatus()
{
    if (m_statusLabel) m_statusLabel->setText(QString::fromStdString(m_sim->statusText()));
}

void MainWindow::rebuildPeriphCharUI()
{
    if (!m_periphCharLayout) return;

    while (QLayoutItem *item = m_periphCharLayout->takeAt(0)) {
        if (QWidget *w = item->widget()) delete w;
        delete item;
    }

    const auto &chars = m_sim->periphChars();
    if (chars.empty()) {
        m_periphCharLayout->addWidget(new QLabel("  (waiting for App Peripheral to advertise...)"));
        return;
    }

    m_periphCharLayout->addWidget(new QLabel(QString::fromStdString("App Peripheral: " + m_sim->periphName())));
    for (size_t i = 0; i < chars.size(); i++) {
        auto *row = new QWidget(m_periphCharContainer);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(4);
        rowLayout->addWidget(new QLabel(QString::fromStdString(chars[i].chr.substr(0, 8) + "...")));
        rowLayout->addWidget(new QLabel("Val:"));
        auto *ve = new QLineEdit(m_periphCharContainer);
        ve->setText(QString::fromStdString(chars[i].value));
        ve->setFixedWidth(120);
        rowLayout->addWidget(ve);
        auto *wb = new QPushButton("Write", m_periphCharContainer);
        rowLayout->addWidget(wb);
        rowLayout->addStretch(1);
        QObject::connect(wb, &QPushButton::clicked, this, [this, i, ve]() {
            m_sim->writePeriphChar(i, ve->text().toStdString());
        });
    }
}

void MainWindow::appendLog(const QString &text)
{
    if (m_logEdit) m_logEdit->append(text);
}
