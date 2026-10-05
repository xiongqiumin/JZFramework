#ifndef BTE_STEP_EDIT_DIALOG_H_
#define BTE_STEP_EDIT_DIALOG_H_

#include "JZDialog.h"
#include "PackDefine.h"

#include <vector>

class JZBoxLayout;
class JZCheckbox;
class JZComboBox;
class JZLineEdit;

// ---------------------------------------------------------------------------
// StepEditDialog —— 单个工步的明细编辑器(对应原版 BTEServer/StepEditDialog)
//
// 原版是 .ui + 动态生成参数行(362 行 .cpp): 一行 = [勾选框] 参数名 + 运算符下拉 + 数值框;
// 换工步类型时按 PackDefine::StepTypeMustHave 启用/禁用相应的行。这里照这个形状做, 差异写在明处:
//   · 参数行不用 .ui, 代码搭(与 StepSettingDialog 同一套路);
//   · **没搬** StepTypeMustHave 那张"每种工步类型必须有哪些参数"的细表 —— 骨架先按大类:
//     静置/停止 只让编"时间", 其它类型全部可编(要细化时照原版那张表加判断即可);
//   · 时间在界面上是**秒**, 协议是 ms(与原版界面一致); 整型类(起始工步/循环/跳转)直接填整数。
//
// 进出: StepSettingDialog 的工步列表选中一行 -> 点"编辑"弹它, 确定后把结果写回该行。
// ---------------------------------------------------------------------------
class StepEditDialog : public JZDialog
{
public:
    explicit StepEditDialog(JZWidget* parent = nullptr);

    void setStep(const StruSetStepInfo& step);          // 回填界面
    const StruSetStepInfo& step() const { return m_step; }

private:
    // 参数行的取值方式
    enum ParamKind
    {
        KindNum  = 0,   // 数值(电压/电流/容量/温度/…)
        KindTime = 1,   // 时间: 界面秒 -> 协议 ms
        KindInt  = 2,   // 整型(起始工步/循环/跳转)
    };

    struct ParamRow
    {
        int         param = -1;
        ParamKind   kind = KindNum;
        JZCheckbox* box = nullptr;
        JZComboBox* op = nullptr;
        JZLineEdit* edit = nullptr;
    };

    void buildUi();
    void stepToUi();                 // 原版 stepToUi
    bool uiToStep();                 // 原版 uiToStep
    void applyTypeEnable();          // 按工步类型启用/禁用参数行(原版 isVaildParam 的简化)

    ParamRow addParamRow(JZBoxLayout* layout, const char* name, int param, ParamKind kind);

    JZComboBox* m_boxType = nullptr;     // 工步类型(下标 -> 值 见 m_typeValues)
    std::vector<int> m_typeValues;       // 下拉第 i 项对应的 STEP_TYPE_*
    std::vector<ParamRow> m_rows;

    StruSetStepInfo m_step;
};

#endif // BTE_STEP_EDIT_DIALOG_H_
