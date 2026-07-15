#ifndef JZ_MAIN_WINDOW_H_
#define JZ_MAIN_WINDOW_H_

#include <QMainWindow>
#include <QStackedWidget>
#include <QLabel>
#include <QTextEdit>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <vector>

class JZSimulator;
class JZSimulatorBridge;

// 视图层: 仅负责 Qt 界面, 通过 JZSimulator 的接口读写数据/下发命令,
// 并通过 JZSimulatorBridge 的 Qt 信号接收模型层的状态/日志通知。
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(JZSimulator *sim, JZSimulatorBridge *bridge, QWidget *parent = nullptr);
    ~MainWindow() override;

private:
    void buildUI();
    void buildPeripheralPage();
    void buildCentralPage();
    void buildConfigPage();
    void switchTab(int idx);

    // 模型 -> 视图 刷新
    void refreshStatus();
    void rebuildPeriphCharUI();
    void appendLog(const QString &text);

    JZSimulator *m_sim = nullptr;
    JZSimulatorBridge *m_bridge = nullptr;

    QStackedWidget  *m_stack = nullptr;
    std::vector<QPushButton *> m_tabBtns;

    // Peripheral 页
    QLineEdit   *m_nameEdit = nullptr;
    QCheckBox   *m_advSwitch = nullptr;
    QWidget     *m_charContainer = nullptr;
    QVBoxLayout *m_charLayout = nullptr;

    // Central 页
    QWidget     *m_periphCharContainer = nullptr;
    QVBoxLayout *m_periphCharLayout = nullptr;

    // 底部
    QLabel      *m_statusLabel = nullptr;
    QTextEdit   *m_logEdit = nullptr;
};

#endif // !JZ_MAIN_WINDOW_H_
