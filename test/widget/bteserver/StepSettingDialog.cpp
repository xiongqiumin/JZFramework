#include "StepSettingDialog.h"

#include "JZCheckbox.h"
#include "JZLabel.h"
#include "JZLineEdit.h"
#include "JZPushButton.h"
#include "JZSpinBox.h"
#include "JZTableWidget.h"
#include "JZTextEdit.h"
#include "StepEditDialog.h"      // 工步列表里的"编辑"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>

namespace {

double toDouble(const std::string& s)
{
    return s.empty() ? 0.0 : std::atof(s.c_str());
}

std::string numText(double v)
{
    char b[32];
    snprintf(b, sizeof(b), "%g", v);    // 4.2 -> "4.2"; 3000 -> "3000"
    return b;
}

// 12 个"对所有工步统一施加"的扩展参数(原版 uiToStep 里的 extParam)
const int kExtParams[] = {
    PARAM_RecordTimeInterval, PARAM_RecordVoltageChange, PARAM_RecordCurrentChange,
    PARAM_ProtectVoltageLower, PARAM_ProtectVoltageUpper,
    PARAM_ProtectCurrentLower, PARAM_ProtectCurrentUpper,
    PARAM_ProtectTempatureLower, PARAM_ProtectTempatureUpper,
    PARAM_ProtectDelayTime, PARAM_ProtectDetalVolTime, PARAM_ProtectDetalVol,
};
const int kExtParamCount = (int)(sizeof(kExtParams) / sizeof(kExtParams[0]));

// 工步列表里显示一个参数(没勾 = 不下发 ⇒ 显示 "-")
std::string cellNum(const StruSetStepInfo& st, int param)
{
    if (!st.isVaild(param))
        return "-";
    return numText(st.paramList[param].num);
}

std::string cellTime(const StruSetStepInfo& st, int param)   // 协议 ms -> 秒
{
    if (!st.isVaild(param))
        return "-";
    return numText((double)(st.paramList[param].i64 / 1000));
}

std::string cellTypeName(int type)
{
    if (type >= 0 && type < (int)PackDefine::StepNameList.size() && !PackDefine::StepNameList[type].empty())
        return PackDefine::StepNameList[type];
    return std::to_string(type);
}

} // namespace

StepSettingDialog::StepSettingDialog(JZWidget* parent)
    : JZDialog(parent)
{
    buildUi();
}

void StepSettingDialog::setStep(const StruSetStep76Ex& step)
{
    m_step = step;
    initFromStep();
    refreshStepTable();
}

void StepSettingDialog::addSectionTitle(JZWidget* host, JZBoxLayout* layout, const char* text)
{
    JZLabel* lb = new JZLabel(host);
    lb->setText(std::string("— ") + text + " —");
    layout->addWidget(lb, 0);
}

StepSettingDialog::Row StepSettingDialog::addRow(JZWidget* host, JZBoxLayout* layout,
                                                 const char* text, const char* unit1, const char* unit2)
{
    JZWidget* line = new JZWidget(host);
    JZHBoxLayout* h = new JZHBoxLayout(6, 0);

    Row r;

    r.box = new JZCheckbox(line);
    r.box->setText(text);
    h->addWidget(r.box, 0);

    r.edit = new JZLineEdit(line);
    h->addWidget(r.edit, 1);

    if (unit1)
    {
        JZLabel* u1 = new JZLabel(line);
        u1->setText(unit1);
        h->addWidget(u1, 0);
    }

    if (unit2)
    {
        r.edit2 = new JZLineEdit(line);
        h->addWidget(r.edit2, 1);

        JZLabel* u2 = new JZLabel(line);
        u2->setText(unit2);
        h->addWidget(u2, 0);
    }

    line->setLayout(h);
    layout->addWidget(line, 0);
    return r;
}

void StepSettingDialog::buildUi()
{
    resize(560, 660);

    JZVBoxLayout* root = new JZVBoxLayout(6, 10);
    setLayout(root);

    // ---- 工步元信息(原版 lineStartStep / lineUser / lineCellNum / textMomo) ----
    {
        JZWidget* line = new JZWidget(this);
        JZHBoxLayout* h = new JZHBoxLayout(6, 0);

        JZLabel* lb = new JZLabel(line);
        lb->setText("起始工步");
        h->addWidget(lb, 0);

        m_startStep = new JZSpinBox(line);
        m_startStep->setRange(1, PARAM_COUNT);      // 与原版一致的上限口径(1..64)
        m_startStep->setValue(1);
        h->addWidget(m_startStep, 1);

        line->setLayout(h);
        root->addWidget(line, 0);
    }
    {
        JZWidget* line = new JZWidget(this);
        JZHBoxLayout* h = new JZHBoxLayout(6, 0);

        JZLabel* lb = new JZLabel(line);
        lb->setText("创建者");
        h->addWidget(lb, 0);

        m_creator = new JZLineEdit(line);
        h->addWidget(m_creator, 1);

        line->setLayout(h);
        root->addWidget(line, 0);
    }
    {
        JZWidget* line = new JZWidget(this);
        JZHBoxLayout* h = new JZHBoxLayout(6, 0);

        JZLabel* lb = new JZLabel(line);
        lb->setText("批号/电池型号");
        h->addWidget(lb, 0);

        m_cellBatch = new JZLineEdit(line);
        h->addWidget(m_cellBatch, 1);

        line->setLayout(h);
        root->addWidget(line, 0);
    }
    {
        JZLabel* lb = new JZLabel(this);
        lb->setText("备注");
        root->addWidget(lb, 0);

        m_memo = new JZTextEdit(this);
        root->addWidget(m_memo, 1);        // 备注吃掉剩余高度
    }

    // ---- 工步列表(原版 tableWidget; 只读展示, 改动都走 StepEditDialog) ----
    addSectionTitle(this, root, "工步列表");
    {
        m_stepTable = new JZTableWidget(this);

        std::vector<std::string> head;
        head.push_back("序号");       head.push_back("工步");        head.push_back("时间(s)");
        head.push_back("电压(V)");    head.push_back("电流(A)");     head.push_back("截止压(V)");
        head.push_back("截止流(A)");  head.push_back("容量(Ah)");    head.push_back("温度(℃)");
        m_stepTable->setColumnCount((int)head.size());
        m_stepTable->setHorizontalHeaderLabels(head);
        root->addWidget(m_stepTable, 1);

        JZWidget* line = new JZWidget(this);
        JZHBoxLayout* h = new JZHBoxLayout(6, 0);

        // 按钮都作用于"表格当前选中行"(原版 btnEdit/Up/Down/Add/Del)
        auto addBtn = [this, line, h](const char* text, std::function<void(int)> fn) {
            JZPushButton* b = new JZPushButton(text, line);
            connect(&b->sigClicked, [this, fn]() {
                fn(m_stepTable ? m_stepTable->currentRow() : -1);
            });
            h->addWidget(b, 0);
        };

        addBtn("编辑",     [this](int row) { editStepAt(row); });
        addBtn("上移",     [this](int row) { moveStep(row, -1); });
        addBtn("下移",     [this](int row) { moveStep(row, +1); });
        addBtn("插入",     [this](int row) { insertStepAfter(row); });
        addBtn("删除",     [this](int row) { removeStepAt(row); });
        addBtn("恢复默认", [this](int) {
            m_step.genDefaultStep();
            refreshStepTable();
        });

        line->setLayout(h);
        root->addWidget(line, 0);
    }

    // ---- 记录条件(原版 boxRecordTimeGap / boxRecordVoltGap / boxRecordCurrentGap) ----
    addSectionTitle(this, root, "记录条件");
    m_recordTime = addRow(this, root, "时间间隔", "秒");
    m_recordVolt = addRow(this, root, "电压突变", "V");
    m_recordCur  = addRow(this, root, "电流突变", "A");

    // ---- 安全保护(原版 boxSecurity*) ----
    addSectionTitle(this, root, "安全保护");
    m_voltLow  = addRow(this, root, "电压下限", "V");
    m_voltUp   = addRow(this, root, "电压上限", "V");
    m_curLow   = addRow(this, root, "电流下限", "A");
    m_curUp    = addRow(this, root, "电流上限", "A");
    m_tempLow  = addRow(this, root, "温度下限", "℃");
    m_tempUp   = addRow(this, root, "温度上限", "℃");
    m_delay    = addRow(this, root, "延时保护", "秒");

    // ---- 化成保护(原版 boxProtectDetal + 时间/电压) ----
    addSectionTitle(this, root, "化成保护");
    m_detal = addRow(this, root, "变化率电压", "V", "检测时间(秒)");

    // ---- 按钮: 确定(=原版 btnOk "启动") / 取消 ----
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

    refreshStepTable();          // 初始刷一次(stepList 可能还是空的, 无害)
}

// 工步 -> 界面(原版 initStep + stepParamToUi 的简化: 取第一个非"停止"工步当代表 ——
// 原版这些扩展参数本来就是对所有工步一致的)
void StepSettingDialog::initFromStep()
{
    m_startStep->setValue(startStepIndex);
    m_creator->setText(creator);
    m_cellBatch->setText(cellBatch);
    m_memo->setText(memo);

    const StruSetStepInfo* ref = nullptr;
    for (size_t i = 0; i < m_step.stepList.size(); ++i)
    {
        if (m_step.stepList[i].StepType != STEP_TYPE_STOP)
        {
            ref = &m_step.stepList[i];
            break;
        }
    }
    if (!ref)
        return;

    // 数值类: 直接搬
    auto fillNum = [ref](Row& r, int idx) {
        if (!ref->isVaild(idx))
            return;
        r.box->setChecked(true);
        r.edit->setText(numText(ref->paramList[idx].num));
    };

    // 时间类(协议是 ms, 界面是秒)
    auto fillSec = [ref](Row& r, int idx) {
        if (!ref->isVaild(idx))
            return;
        r.box->setChecked(true);
        r.edit->setText(numText((double)(ref->paramList[idx].i64 / 1000)));
    };

    fillSec(m_recordTime, PARAM_RecordTimeInterval);
    fillNum(m_recordVolt, PARAM_RecordVoltageChange);
    fillNum(m_recordCur,  PARAM_RecordCurrentChange);
    fillNum(m_voltLow,    PARAM_ProtectVoltageLower);
    fillNum(m_voltUp,     PARAM_ProtectVoltageUpper);
    fillNum(m_curLow,     PARAM_ProtectCurrentLower);
    fillNum(m_curUp,      PARAM_ProtectCurrentUpper);
    fillNum(m_tempLow,    PARAM_ProtectTempatureLower);
    fillNum(m_tempUp,     PARAM_ProtectTempatureUpper);
    fillSec(m_delay,      PARAM_ProtectDelayTime);
    fillNum(m_detal,      PARAM_ProtectDetalVol);

    if (ref->isVaild(PARAM_ProtectDetalVolTime) && m_detal.edit2)
    {
        m_detal.box->setChecked(true);
        m_detal.edit2->setText(numText((double)(ref->paramList[PARAM_ProtectDetalVolTime].i64 / 1000)));
    }
}

// 界面 -> 工步(照原版 uiToStep)
bool StepSettingDialog::uiToStep()
{
    startStepIndex = m_startStep->value();
    creator = m_creator->text();
    cellBatch = m_cellBatch->text();
    memo = m_memo->text();

    const int64_t recordTimeMs = (int64_t)(toDouble(m_recordTime.edit->text()) * 1000);
    const double  recordVolt   = toDouble(m_recordVolt.edit->text());
    const double  recordCur    = toDouble(m_recordCur.edit->text());

    const double voltLow  = toDouble(m_voltLow.edit->text());
    const double voltUp   = toDouble(m_voltUp.edit->text());
    const double curLow   = toDouble(m_curLow.edit->text());
    const double curUp    = toDouble(m_curUp.edit->text());
    const double tempLow  = toDouble(m_tempLow.edit->text());
    const double tempUp   = toDouble(m_tempUp.edit->text());
    const int64_t delayMs = (int64_t)(toDouble(m_delay.edit->text()) * 1000);

    const double detalVol  = toDouble(m_detal.edit->text());
    const int64_t detalMs  = m_detal.edit2
        ? (int64_t)(toDouble(m_detal.edit2->text()) * 1000) : 0;

    for (size_t i = 0; i < m_step.stepList.size(); ++i)
    {
        StruSetStepInfo& st = m_step.stepList[i];

        // 原版: 先把这 12 个扩展参数全清掉, 再按勾选框逐个设
        for (int j = 0; j < kExtParamCount; ++j)
        {
            if (kExtParams[j] < 0 || kExtParams[j] >= (int)st.paramList.size())
                continue;
            st.paramList[kExtParams[j]].vaild = false;
            st.paramList[kExtParams[j]].num = 0.0;
            st.paramList[kExtParams[j]].i64 = 0;
        }

        if (st.StepType == STEP_TYPE_STOP)
            continue;                        // 停止工步不参与(原版同)

        if (m_recordTime.box->isChecked())
            st.setParamI64(PARAM_RecordTimeInterval, OP_BIG_EQU, recordTimeMs);
        if (m_recordVolt.box->isChecked())
            st.setParam(PARAM_RecordVoltageChange, OP_BIG_EQU, recordVolt);
        if (m_recordCur.box->isChecked())
            st.setParam(PARAM_RecordCurrentChange, OP_BIG_EQU, recordCur);

        if (m_voltLow.box->isChecked())
            st.setParam(PARAM_ProtectVoltageLower, OP_BIG_EQU, voltLow);
        if (m_voltUp.box->isChecked())
            st.setParam(PARAM_ProtectVoltageUpper, OP_SMALL_EQU, voltUp);
        if (m_curLow.box->isChecked())
            st.setParam(PARAM_ProtectCurrentLower, OP_BIG_EQU, curLow);
        if (m_curUp.box->isChecked())
            st.setParam(PARAM_ProtectCurrentUpper, OP_SMALL_EQU, curUp);
        if (m_tempLow.box->isChecked())
            st.setParam(PARAM_ProtectTempatureLower, OP_BIG_EQU, tempLow);
        if (m_tempUp.box->isChecked())
            st.setParam(PARAM_ProtectTempatureUpper, OP_SMALL_EQU, tempUp);
        if (m_delay.box->isChecked())
            st.setParamI64(PARAM_ProtectDelayTime, OP_BIG_EQU, delayMs);

        if (m_detal.box->isChecked())
        {
            st.setParam(PARAM_ProtectDetalVol, OP_BIG_EQU, detalVol);
            st.setParamI64(PARAM_ProtectDetalVolTime, OP_BIG_EQU, detalMs);
        }
    }

    m_step.updateStepLine();     // 原版收尾: 重排工步行号/工步号
    return true;
}

// ---------------------------------------------------------------------------
// 工步列表(原版 tableWidget + btnEdit/Up/Down/Add/Del)
//   表格只读展示 —— 真正的编辑在 StepEditDialog 里做, 改完把结果写回 stepList。
//   ⚠ 原版支持双击进编辑; 引擎的 JZTableWidget 目前没有双击信号, 所以只挂了"编辑"按钮
//     (等表格补双击事件再顺手加上)。
// ---------------------------------------------------------------------------
void StepSettingDialog::refreshStepTable()
{
    if (!m_stepTable)
        return;

    const int n = (int)m_step.stepList.size();
    m_stepTable->setRowCount(n);

    for (int i = 0; i < n; ++i)
    {
        const StruSetStepInfo& st = m_step.stepList[i];

        m_stepTable->setItemText(i, 0, std::to_string(i + 1));
        m_stepTable->setItemText(i, 1, cellTypeName(st.StepType));
        m_stepTable->setItemText(i, 2, cellTime(st, PARAM_Time));
        m_stepTable->setItemText(i, 3, cellNum(st, PARAM_Voltage));
        m_stepTable->setItemText(i, 4, cellNum(st, PARAM_Current));
        m_stepTable->setItemText(i, 5, cellNum(st, PARAM_StopVoltage));
        m_stepTable->setItemText(i, 6, cellNum(st, PARAM_StopCurrent));
        m_stepTable->setItemText(i, 7, cellNum(st, PARAM_Capacity));
        m_stepTable->setItemText(i, 8, cellNum(st, PARAM_Tempature));
    }
}

// 弹 StepEditDialog 改第 idx 个工步
void StepSettingDialog::editStepAt(int idx)
{
    if (idx < 0 || idx >= (int)m_step.stepList.size())
        return;

    StepEditDialog dlg(this);
    dlg.setStep(m_step.stepList[idx]);
    if (dlg.exec() != JZDialog::Accepted)
        return;

    m_step.stepList[idx] = dlg.step();
    m_step.updateStepLine();          // 工步号跟着行号走
    refreshStepTable();
}

void StepSettingDialog::moveStep(int idx, int delta)
{
    const int to = idx + delta;
    if (idx < 0 || idx >= (int)m_step.stepList.size())
        return;
    if (to < 0 || to >= (int)m_step.stepList.size())
        return;

    std::swap(m_step.stepList[idx], m_step.stepList[to]);
    m_step.updateStepLine();
    refreshStepTable();

    if (m_stepTable)
        m_stepTable->setCurrentCell(to, 0);      // 选中跟着走, 方便连点
}

void StepSettingDialog::insertStepAfter(int idx)
{
    StruSetStepInfo st;
    st.StepType = STEP_TYPE_IDLE;                // 新行给"静置"

    const int at = (idx < 0) ? (int)m_step.stepList.size() : idx + 1;
    if (at > (int)m_step.stepList.size())
        return;

    m_step.stepList.insert(m_step.stepList.begin() + at, st);
    m_step.updateStepLine();
    refreshStepTable();

    if (m_stepTable)
        m_stepTable->setCurrentCell(at, 0);
}

void StepSettingDialog::removeStepAt(int idx)
{
    if (idx < 0 || idx >= (int)m_step.stepList.size())
        return;
    if (m_step.stepList.size() <= 1)
        return;                                  // 至少留一个(原版也不让删空)

    m_step.stepList.erase(m_step.stepList.begin() + idx);
    m_step.updateStepLine();
    refreshStepTable();
}
