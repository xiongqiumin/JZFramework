#ifndef BTE_STEP_SETTING_DIALOG_H_
#define BTE_STEP_SETTING_DIALOG_H_

#include "JZDialog.h"
#include "JZLayout.h"
#include "PackDefine.h"

#include <string>

class JZBoxLayout;
class JZCheckbox;
class JZLineEdit;
class JZSpinBox;
class JZTableWidget;
class JZTextEdit;

// ---------------------------------------------------------------------------
// StepSettingDialog —— 工步设置对话框(对应原版 BTEServer/StepSettingDialog)
//
// 原版是 Qt Designer 的 .ui(589 行、二十来组参数), 这里是**用代码搭**的骨架版:
//   · 只做原版主界面真会用到的那几组 —— 起始工步/创建者/批号/备注、记录条件(时间间隔/
//     电压突变/电流突变)、安全保护(电压上下/电流上下/温度上下/延时)、化成保护(变化率
//     电压 + 检测时间)。
//   · **没做**的几组(原版有、骨架没有对应概念): 辅助通道那一整组(AuxXXX)、红外扫描
//     (IRDCXXX)、平台点监测(PlatVoltage1..3)、循环/跳转/脉冲之类。要补时照 uiToStep 里的
//     extParam 表加下标即可。
//
// 语义与原版一致(这两条是关键):
//   1. startStepIndex/creator/cellBatch/memo 是**整套工步的元信息**, 不属于某一工步;
//   2. 记录条件/安全保护/化成保护是**对工步列表里每个工步统一施加**的 —— 原版 uiToStep()
//      遍历 stepList, 对每个工步先清掉那 12 个扩展参数再按勾选框设值, STEP_TYPE_STOP 跳过。
//
// 界面文本标签对应原版控件: lineStartStep/lineUser/lineCellNum/textMomo + 各组 box*/line*。
// ---------------------------------------------------------------------------
class StepSettingDialog : public JZDialog
{
public:
    explicit StepSettingDialog(JZWidget* parent = nullptr);

    // 待编辑的整套工步(进来时用它回填界面; 不设则按空工步处理)
    void setStep(const StruSetStep76Ex& step);

    // 确定后的结果(accept 之后取)
    const StruSetStep76Ex& step() const { return m_step; }
    int         startStepIndex = 1;      // 原版 BTEStepInfo::startStepIndex
    std::string creator;                 // 原版 creator
    std::string cellBatch;               // 原版 cellBatch(批号/电池型号)
    std::string memo;                    // 原版 memo

private:
    void buildUi();
    void initFromStep();                 // 工步 -> 界面(原版 initStep/stepParamToUi)
    bool uiToStep();                     // 界面 -> 工步(原版 uiToStep; 校验不过返回 false)

    void refreshStepTable();             // 工步列表(只读展示; 原版 tableWidget)
    void editStepAt(int idx);            // 弹 StepEditDialog 改第 idx 个工步(原版 btnEdit)
    void moveStep(int idx, int delta);   // 上移/下移(原版 btnUp/btnDown)
    void insertStepAfter(int idx);       // 插入(原版 btnAdd)
    void removeStepAt(int idx);          // 删除(原版 btnDel)

    // 一小组"勾选框 + 一个数值输入"
    struct Row
    {
        JZCheckbox* box = nullptr;
        JZLineEdit* edit = nullptr;      // 第一段数值
        JZLineEdit* edit2 = nullptr;     // 第二段数值(化成保护那组用)
    };

    // 往布局里加一行: [勾选框] 文本 + 数值框(+ 可选第二个数值框)
    Row addRow(JZWidget* host, JZBoxLayout* layout, const char* text,
               const char* unit1, const char* unit2 = nullptr);
    void addSectionTitle(JZWidget* host, JZBoxLayout* layout, const char* text);

    // 元信息
    JZSpinBox*  m_startStep = nullptr;
    JZLineEdit* m_creator = nullptr;
    JZLineEdit* m_cellBatch = nullptr;
    JZTextEdit* m_memo = nullptr;

    // 记录条件
    Row m_recordTime;      // 时间间隔(秒)
    Row m_recordVolt;      // 电压突变
    Row m_recordCur;       // 电流突变

    // 安全保护
    Row m_voltLow;
    Row m_voltUp;
    Row m_curLow;
    Row m_curUp;
    Row m_tempLow;
    Row m_tempUp;
    Row m_delay;           // 延时(秒)

    // 化成保护
    Row m_detal;           // 变化率电压 + 检测时间(秒)

    JZTableWidget* m_stepTable = nullptr;    // 工步列表(只读; 改动都走 StepEditDialog)

    StruSetStep76Ex m_step;
};

#endif // BTE_STEP_SETTING_DIALOG_H_
