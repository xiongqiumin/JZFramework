#include "StepEditDialog.h"

#include "JZCheckbox.h"
#include "JZComboBox.h"
#include "JZLabel.h"
#include "JZLayout.h"
#include "JZLineEdit.h"
#include "JZPushButton.h"

#include <cstdio>
#include <cstdlib>

namespace {

double toDouble(const std::string& s)
{
    return s.empty() ? 0.0 : std::atof(s.c_str());
}

std::string numText(double v)
{
    char b[32];
    snprintf(b, sizeof(b), "%g", v);
    return b;
}

// 运算符下拉: 显示文本 <-> OP_*
const char* kOpText[] = { "==", "!=", ">", ">=", "<", "<=" };
const uint8_t kOpValue[] = { OP_EQU, OP_NEQ, OP_BIG, OP_BIG_EQU, OP_SMALL, OP_SMALL_EQU };
const int kOpCount = (int)(sizeof(kOpValue) / sizeof(kOpValue[0]));

int opIndex(uint8_t op)
{
    for (int i = 0; i < kOpCount; ++i)
    {
        if (kOpValue[i] == op)
            return i;
    }
    return 0;
}

} // namespace

StepEditDialog::StepEditDialog(JZWidget* parent)
    : JZDialog(parent)
{
    buildUi();
}

void StepEditDialog::setStep(const StruSetStepInfo& step)
{
    m_step = step;
    stepToUi();
}

StepEditDialog::ParamRow StepEditDialog::addParamRow(JZBoxLayout* layout, const char* name,
                                                     int param, ParamKind kind)
{
    JZWidget* line = new JZWidget(this);
    JZHBoxLayout* h = new JZHBoxLayout(6, 0);

    ParamRow r;
    r.param = param;
    r.kind = kind;

    r.box = new JZCheckbox(line);
    r.box->setText(name);
    h->addWidget(r.box, 0);

    r.op = new JZComboBox(line);
    for (int i = 0; i < kOpCount; ++i)
        r.op->addItem(kOpText[i]);
    r.op->setCurrentIndex(0);
    h->addWidget(r.op, 0);

    r.edit = new JZLineEdit(line);
    h->addWidget(r.edit, 1);

    line->setLayout(h);
    layout->addWidget(line, 0);

    m_rows.push_back(r);
    return r;
}

void StepEditDialog::buildUi()
{
    resize(520, 620);

    JZVBoxLayout* root = new JZVBoxLayout(5, 10);
    setLayout(root);

    // ---- 工步类型 ----
    {
        JZWidget* line = new JZWidget(this);
        JZHBoxLayout* h = new JZHBoxLayout(6, 0);

        JZLabel* lb = new JZLabel(line);
        lb->setText("工步类型");
        h->addWidget(lb, 0);

        m_boxType = new JZComboBox(line);
        // 下标 = STEP_TYPE_* 里的**有效项**; m_typeValues 记下每个下拉项对应的类型值
        for (int t = 0; t < (int)PackDefine::StepNameList.size(); ++t)
        {
            if (PackDefine::StepNameList[t].empty())
                continue;                       // 表里有空项(没定义的类型)
            m_boxType->addItem(PackDefine::StepNameList[t]);
            m_typeValues.push_back(t);
        }
        h->addWidget(m_boxType, 1);

        line->setLayout(h);
        root->addWidget(line, 0);

        // 换类型 -> 重新决定哪些参数行可编(原版 on_boxType_currentIndexChanged)
        connect(&m_boxType->currentIndexChanged, [this](int) { applyTypeEnable(); });
    }

    // ---- 参数行(顺序照原版大类: 时间/电压/电流/截止/容量/温度 + 功率/电阻/ΔV + 三个整型) ----
    addParamRow(root, "时间",     PARAM_Time,           KindTime);
    addParamRow(root, "电压",     PARAM_Voltage,        KindNum);
    addParamRow(root, "电流",     PARAM_Current,        KindNum);
    addParamRow(root, "截止电压", PARAM_StopVoltage,    KindNum);
    addParamRow(root, "截止电流", PARAM_StopCurrent,    KindNum);
    addParamRow(root, "容量",     PARAM_Capacity,       KindNum);
    addParamRow(root, "温度",     PARAM_Tempature,      KindNum);
    addParamRow(root, "功率",     PARAM_Power,          KindNum);
    addParamRow(root, "电阻",     PARAM_Resistance,     KindNum);
    addParamRow(root, "电压变化", PARAM_VoltageChange,  KindNum);
    addParamRow(root, "起始工步", PARAM_BeginStepID,    KindInt);
    addParamRow(root, "循环次数", PARAM_CountLoop,      KindInt);
    addParamRow(root, "跳转行号", PARAM_JumpLine,       KindInt);

    // ---- 按钮 ----
    {
        JZWidget* line = new JZWidget(this);
        JZHBoxLayout* h = new JZHBoxLayout(8, 0);

        JZPushButton* ok = new JZPushButton("确定", line);
        JZPushButton* cancel = new JZPushButton("取消", line);

        connect(&ok->sigClicked, [this]() {
            if (uiToStep())
                accept();
        });
        connect(&cancel->sigClicked, [this]() { reject(); });

        h->addWidget(ok, 1);
        h->addWidget(cancel, 1);
        line->setLayout(h);
        root->addWidget(line, 0);
    }
}

// 工步 -> 界面(原版 stepToUi)
void StepEditDialog::stepToUi()
{
    // 类型: 在 m_typeValues 里找到当前 StepType 对应的下拉下标
    for (size_t i = 0; i < m_typeValues.size(); ++i)
    {
        if (m_typeValues[i] == (int)m_step.StepType)
        {
            m_boxType->setCurrentIndex((int)i);
            break;
        }
    }

    for (size_t i = 0; i < m_rows.size(); ++i)
    {
        ParamRow& r = m_rows[i];
        r.box->setChecked(false);
        r.edit->setText("");
        r.op->setCurrentIndex(0);

        if (r.param < 0 || r.param >= (int)m_step.paramList.size() || !m_step.paramList[r.param].vaild)
            continue;

        const StruSetStepParam& p = m_step.paramList[r.param];
        r.box->setChecked(true);
        r.op->setCurrentIndex(opIndex(p.op));

        if (r.kind == KindTime)
            r.edit->setText(numText((double)(p.i64 / 1000)));     // 协议 ms -> 界面秒
        else if (r.kind == KindInt)
            r.edit->setText(std::to_string(p.i64));
        else
            r.edit->setText(numText(p.num));
    }

    applyTypeEnable();
}

// 界面 -> 工步(原版 uiToStep)
bool StepEditDialog::uiToStep()
{
    const int ti = m_boxType->currentIndex();
    if (ti >= 0 && ti < (int)m_typeValues.size())
        m_step.StepType = (uint8_t)m_typeValues[ti];

    for (size_t i = 0; i < m_rows.size(); ++i)
    {
        ParamRow& r = m_rows[i];
        if (r.param < 0 || r.param >= (int)m_step.paramList.size())
            continue;

        // 不勾 = 这个参数不下发(原版同: 只写勾了的)
        if (!r.box->isChecked())
        {
            m_step.paramList[r.param].vaild = false;
            continue;
        }

        const uint8_t op = kOpValue[opIndex(r.op->currentIndex())];

        if (r.kind == KindTime)
            m_step.setParamI64(r.param, op, (int64_t)(toDouble(r.edit->text()) * 1000.0));
        else if (r.kind == KindInt)
            m_step.setParamI64(r.param, op, (int64_t)std::atoll(r.edit->text().c_str()));
        else
            m_step.setParam(r.param, op, toDouble(r.edit->text()));
    }

    return true;
}

// 按工步类型决定哪些参数行可编。
// ⚠ 原版用 PackDefine::StepTypeMustHave(每种类型"必须有哪些参数"的细表); 骨架没搬那张表,
//   先按大类: 静置/停止 只留"时间", 其它类型全部可编。要细化时照原版那张表加判断即可。
void StepEditDialog::applyTypeEnable()
{
    const int ti = m_boxType->currentIndex();
    const int type = (ti >= 0 && ti < (int)m_typeValues.size()) ? m_typeValues[ti] : STEP_TYPE_NULL;

    const bool idleLike = (type == STEP_TYPE_IDLE || type == STEP_TYPE_STOP || type == STEP_TYPE_NULL);

    for (size_t i = 0; i < m_rows.size(); ++i)
    {
        ParamRow& r = m_rows[i];
        const bool on = !idleLike || r.param == PARAM_Time;

        r.box->setEnabled(on);
        r.op->setEnabled(on);
        r.edit->setEnabled(on);
    }
}
