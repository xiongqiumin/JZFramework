#pragma once

#include "JZWidget.h"
#include "JZLabel.h"
#include "JZPushButton.h"
#include "JZSlider.h"
#include "JZProgressBar.h"
#include "JZSwitch.h"
#include "JZCheckbox.h"
#include "JZRadioButton.h"
#include "JZListWidget.h"
#include "JZComboBox.h"
#include "JZArcWidget.h"
#include "JZTextEdit.h"
#include "JZLedWidget.h"
#include "JZRoller.h"
#include "JZTableWidget.h"
#include "JZImageLabel.h"
#include "JZStackedWidget.h"
#include "JZColor.h"

#include <vector>
#include <string>

// 挂载所有已实现控件: 左侧 JZListWidget 选择, 右侧 JZStackedWidget 预览, 底部日志
class MainWindow : public JZWidget
{
public:
    explicit MainWindow(JZWidget* parent);
    ~MainWindow();

private:
    void setupUi();
    JZWidget* makePage();
    void buildPages();

    virtual void paintEvent(JZPainterEvent *event) override;

    // 日志
    void log(const std::string& line);

    // 选择
    void onSelectChanged(int idx);

    // 通用信号槽
    void onLogVoid();
    void onLogInt(int v);
    void onLogBool(bool b);
    void onLogString(const std::string& s);

    // 交互演示
    void onPbInc();
    void onLedToggle();
    void onLogTextEdit();
    void onLabelUpdate();

    JZListWidget    *m_list = nullptr;
    JZWidget        *m_top = nullptr;
    JZStackedWidget *m_preview = nullptr;
    JZTextEdit      *m_log = nullptr;
    std::string      m_logText;
    std::vector<std::string> m_names;

    // 交互演示用到的控件引用
    JZProgressBar *m_pb = nullptr;
    int  m_pbVal = 0;
    JZLedWidget *m_led = nullptr;
    JZTextEdit *m_te = nullptr;
    JZLabel    *m_labelDemo = nullptr;
};
