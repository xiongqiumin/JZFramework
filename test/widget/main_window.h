#pragma once

#include "JZWindow.h"
#include "JZLabel.h"
#include "JZPushButton.h"
#include "JZSlider.h"
#include "JZProgressBar.h"
#include "JZSwitch.h"
#include "JZCheckbox.h"
#include "JZRadioButton.h"
#include "JZListWidget.h"
#include "JZComboBox.h"
#include "JZTextEdit.h"
#include "JZTableWidget.h"
#include "JZImageLabel.h"
#include "JZStackedWidget.h"
#include "JZColor.h"

#include <vector>
#include <string>

// 挂载所有已实现控件: 左侧 JZListWidget 选择, 右侧 JZStackedWidget 预览, 底部日志
class MainWindow : public JZWindow
{
public:
    explicit MainWindow(JZWidget* parent = nullptr);
    ~MainWindow();

private:
    void setupUi();
    JZWidget* makePage();
    void buildPages();

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
    void onLogTextEdit();
    void onLabelUpdate();
    void onDialogTest();

    JZListWidget    *m_list = nullptr;
    JZWidget        *m_top = nullptr;
    JZStackedWidget *m_preview = nullptr;
    JZTextEdit      *m_log = nullptr;
    std::string      m_logText;
    std::vector<std::string> m_names;

    // 交互演示用到的控件引用
    JZProgressBar *m_pb = nullptr;
    int  m_pbVal = 0;
    JZTextEdit *m_te = nullptr;
    JZLabel    *m_labelDemo = nullptr;
};
