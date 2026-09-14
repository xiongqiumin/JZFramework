// ---------------------------------------------------------------------------
// StepSettingDialog.js —— 对应 C++ BTEServer/StepSettingDialog.h + StepSettingDialog.cpp
//
// 工步编辑主对话框: 工步表(新增/删除/编辑/上下移/重置) + 记录条件与保护参数 + 导入导出 .step。
// (StepTableHelper 也定义在 C++ 的 StepSettingDialog.h 里, 一并导出)
//
// Qt -> JS: 出参 double& var -> 写入传入的 vars 对象(保持 C++ 里 || 短路链的写法);
//           qSwap -> 临时变量交换; QDataStream 的 << / >> -> write()/read()
//           (与 BTEProject.js 的约定一致)。
// ---------------------------------------------------------------------------

import { tr, sprintf } from './BTEServer.js';
// 注: C++ 侧 StepInfo(PackDefine.cpp) 与 PackDefine.h 的常量在同一个头文件里, 翻译成分文件后
//     `stru_BTS_CMD_SET_STEP76_Ex` 在 ./structs.js; structs.js 又 `export *` 了 PackDefine.js 的全部
//     导出, 所以这里统一从 './structs.js' 取即可(原来是 './PackDefine.js' → 找不到该导出)。
import { stru_BTS_CMD_SET_STEP76_Ex } from './structs.js';
import { OP_BIG_EQU, OP_EQU, OP_SMALL_EQU, PARAM_ProtectCurrentLower, PARAM_ProtectCurrentUpper, PARAM_ProtectDelayTime, PARAM_ProtectDetalVol, PARAM_ProtectDetalVolTime, PARAM_ProtectTempatureLower, PARAM_ProtectTempatureUpper, PARAM_ProtectVoltageLower, PARAM_ProtectVoltageUpper, PARAM_RecordCurrentChange, PARAM_RecordTimeInterval, PARAM_RecordVoltageChange, PackDefine, STEP_TYPE_7, STEP_TYPE_LOOP, STEP_TYPE_STOP } from './PackDefine.js';
import { DataTrans } from './DataTrans.js';
import { btStepInfoToJson, btStepInfoSetJson, bytesToText } from './storage.js';
import { g_project } from './BTEProject.js';
// 注: BTEStepInfo 在 C++ 里属于 BTERecordDataBase.h, JS 侧同 (BTEProject.js 只导出 g_project/BTEProject/DeviceData/...)
import { BTEStepInfo } from './BTERecordDataBase.js';
import { StepEditDialog } from './StepEditDialog.js';
import { fs } from 'core/dist/index.js';
import { STEPSETTINGDIALOG_UI_XML } from './ui/STEPSETTINGDIALOG_UI_XML.js';

// Qt 枚举里本文件用到的那一项(值与原 Qt 一致): 原来由 qtcompat 提供。
const Qt = {
    ItemIsSelectable: 1,
};

//工步容量设定上限(Ah),精度百分之一
const MAX_STEP_CAPACITY = 10000.0;
//工步温度设定上限(℃),精度0.5℃
const MAX_STEP_TEMPATURE = 300.0;

//enum { COL_TIME = 2, ... COL_COUNT, };
const COL_TIME = 2;
const COL_VOLT = 3;
const COL_CURRENT = 4;
const COL_STOP_CURRENT = 5;
const COL_STOP_VOLT = 6;
const COL_CAPACITY = 7;
const COL_TEMP_UP = 8;
const COL_COUNT = 9;

// ---------------------------------------------------------------------------
// class StepTableHelper : public QObject
// ---------------------------------------------------------------------------
export class StepTableHelper
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor()
    {
        this.tableWidget = null;   // QTableWidget *tableWidget;
    }

    // void init(QTableWidget *table);
    init(table)
    {
        this.tableWidget = table;

        const strListHeader = [];
        strListHeader.push(tr("工步号"), tr("工步名称"), tr("时间(H:M:S:MS)"), tr("电压(V)"), tr("电流(A)"), tr("截止电流(A)"), tr("截止电压(V)"),
            tr("容量(Ah)"), tr("温度(℃)"));
        // 引擎的 JZTableWidget 只有列数/表头这些结构接口; 选中模式、"不可编辑"、表头显隐
        // 都是框架默认行为, Qt 那三个开关没有对应接口(原调用点见下面注释)。
        this.tableWidget.setColumnCount(strListHeader.length);
        this.tableWidget.setHorizontalHeaderLabels(strListHeader);
        // 原来是: setSelectionMode(SingleSelection) / setSelectionBehavior(SelectRows) / setEditTriggers(NoEditTriggers)
    }

    // void updateTable(const BTEStepInfo &stepInfo);
    updateTable(stepInfo)
    {
        const prec = 2;

        const stru_step = stepInfo.step;
        const step_count = stru_step.stepList.length;
        const item_count = this.tableWidget.columnCount();

        this.tableWidget.setRowCount(step_count);
        for (let row = 0; row < step_count; row++)
        {
            const step = stru_step.stepList[row];

            const tableItem = new Array(COL_COUNT).fill(null);   // QTableWidgetItem *tableItem[32] = {0};
            for (let i = 0; i < item_count; i++)
            {
                tableItem[i] = new QTableWidgetItem();
                if (row === step_count - 1) //停止
                {
                    const flag = tableItem[i].flags();
                    tableItem[i].setFlags(flag & (~Qt.ItemIsSelectable));
                }
            }
            tableItem[0].setText(String(row + 1));
            tableItem[1].setText(step.stepName());

            if (step.isTimeVaild())
                tableItem[COL_TIME].setText(DataTrans.fromTimeUnit(step.Time()));
            if (step.isVoltageVaild())
                tableItem[COL_VOLT].setText(Number(step.Voltage()).toFixed(prec));
            if (step.isCurrentVaild())
                tableItem[COL_CURRENT].setText(Number(step.Current()).toFixed(prec));
            if (step.isStopCurrentVaild())
                tableItem[COL_STOP_CURRENT].setText(Number(step.StopCurrent()).toFixed(prec));
            if (step.isStopVoltageVaild())
                tableItem[COL_STOP_VOLT].setText(Number(step.StopVoltage()).toFixed(prec));
            if (step.isCapacityVaild())
                tableItem[COL_CAPACITY].setText(Number(step.Capacity()).toFixed(2));
            if (step.isTempatureVaild())
                tableItem[COL_TEMP_UP].setText(Number(step.Tempature()).toFixed(1));

            if (step.StepType === STEP_TYPE_LOOP)
            {
                tableItem[COL_TIME].setText(tr("起始:") + String(step.BeginStepID()));
                tableItem[COL_VOLT].setText(tr("次数:") + String(step.CountLoop()));
            }

            for (let i = 0; i < item_count; i++)
                this.tableWidget.setItem(row, i, tableItem[i]);
        }
    }
}

// ---------------------------------------------------------------------------
// class StepSettingDialog : public QDialog
// ---------------------------------------------------------------------------
export class StepSettingDialog
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // StepSettingDialog(QWidget *parent = Q_NULLPTR);
    constructor(parent)
    {
        // 原 `QDialog.call(this, parent)`(: QDialog(parent)) 已去掉 —— 本类不再"继承 Qt 对话框",
        // 而是**持一个真窗口**(this.ui, 见下), 窗口方法都转发到它。

        this.DeviceString = "";   // QString DeviceString;
        this.itemIndex = 0;       // int itemIndex;
        this.stepInfo = null;     // BTEStepInfo stepInfo;

        // 真 UI: 原版 StepSettingDialog.ui 转换出的 XML; root 是**新窗口**(JZDialog), 不是 this。
        this.ui = uiLoader.load(STEPSETTINGDIALOG_UI_XML);
        // parent 传进来的是"持真窗口的 JS 对象"(如 BTEServer), 真控件在它的 .ui 上; 取对了再挂。
        if (parent) { const pw = parent.ui ? parent.ui : parent; if (pw) this.ui.setParentWidget(pw); }

        this.m_tableHelper = new StepTableHelper();
        this.m_tableHelper.init(this.ui.tableWidget);

        this.ui.btnSet.setVisible(false);
        this.ui.btnBack.setVisible(false);
    }

    // ~StepSettingDialog();
    dispose()
    {
    }

    // void showEvent(QShowEvent *event) override;
    showEvent(event)
    {
        this.initStep();
        QDialog.prototype.showEvent.call(this, event);   // __super::showEvent(event);
    }

    // void stepParamToUi(const stru_BTS_CMD_SET_STEP76_Ex::StepInfo &step, int param_index, QCheckBox *box, QLineEdit *edit);
    stepParamToUi(step, param_index, box, edit)
    {
        if (step.paramList[param_index].vaild)
        {
            box.setChecked(true);
            if (param_index === PARAM_ProtectDelayTime
                || param_index === PARAM_ProtectDetalVolTime)
            {
                const var_ = (step.paramList[param_index].data.toULongLong() / 1000) | 0;   // (int).../1000
                edit.setText(String(var_));
            }
            else
            {
                const var_ = step.paramList[param_index].data.toDouble();
                edit.setText(Number(var_).toFixed(4));
            }
            edit.home(false);
        }
        else
        {
            box.setChecked(false);
            edit.clear();
        }
    }

    // void initStep();
    initStep()
    {
        const prec = 4;

        this.ui.lineStartStep.setValue(this.stepInfo.startStepIndex);

        this.ui.lineUser.setText(this.stepInfo.creator);
        this.ui.lineCellNum.setText(this.stepInfo.cellBatch);
        this.ui.textMomo.setText(this.stepInfo.memo);

        const step = this.stepInfo.step.stepList[0];
        if (step.isRecordTimeIntervalVaild())
        {
            this.ui.boxRecordTimeGap.setChecked(true);
            this.ui.lineRecordTimeGap.setValue((step.RecordTimeInterval() / 1000) | 0);
        }


        //电压下限
        this.stepParamToUi(step, PARAM_RecordVoltageChange, this.ui.boxRecordVoltGap, this.ui.lineRecordVoltGap);
        this.stepParamToUi(step, PARAM_RecordCurrentChange, this.ui.boxRecordCurrentGap, this.ui.lineRecordCurrentGap);

        this.stepParamToUi(step, PARAM_ProtectVoltageLower, this.ui.boxSecurityVoltLow, this.ui.lineSecurityVoltLow);
        this.stepParamToUi(step, PARAM_ProtectVoltageUpper, this.ui.boxSecurityVoltUp, this.ui.lineSecurityVoltUp);

        this.stepParamToUi(step, PARAM_ProtectCurrentLower, this.ui.boxSecurityCurrentLow, this.ui.lineSecurityCurrentLow);
        this.stepParamToUi(step, PARAM_ProtectCurrentUpper, this.ui.boxSecurityCurrentUp, this.ui.lineSecurityCurrentUp);

        this.stepParamToUi(step, PARAM_ProtectTempatureLower, this.ui.boxSecurityTempLow, this.ui.lineSecurityTempLow);
        this.stepParamToUi(step, PARAM_ProtectTempatureUpper, this.ui.boxSecurityTempUp, this.ui.lineSecurityTempUp);

        this.stepParamToUi(step, PARAM_ProtectDelayTime, this.ui.boxSecurityDelay, this.ui.lineSecurityDelay);

        this.stepParamToUi(step, PARAM_ProtectDetalVol, this.ui.boxProtectDetal, this.ui.lineProtectDetalVol);
        this.stepParamToUi(step, PARAM_ProtectDetalVolTime, this.ui.boxProtectDetal, this.ui.lineProtectDetalTime);

        this.updateTable();
    }

    // bool uiToStep();
    uiToStep()
    {
        const pDevice = g_project.deviceData(this.DeviceString);
        const scale = g_project.deviceData(this.DeviceString).scale(this.itemIndex);
        this.stepInfo.startStepIndex = this.ui.lineStartStep.value();
        this.stepInfo.creator = this.ui.lineUser.text();
        this.stepInfo.cellBatch = this.ui.lineCellNum.text();
        this.stepInfo.memo = this.ui.textMomo.toPlainText();

        const maxCurrent = pDevice.maxCurrent() * 1.1 * scale;
        const maxVoltage = pDevice.maxVoltage() * 1.1 * scale;
        const maxCapacity = pDevice.maxCapacity();
        const maxTemp = pDevice.maxTempature();
        const maxDelay = 100000;

        // 出参 double& -> 写在 vars 里(C++ 里是一整条 || 短路链, 这里保持同样写法)
        const vars = {
            recordVoltGap: 0, recordCurrentGap: 0, securityVoltLow: 0, securityVoltUp: 0,
            securityCurrentLow: 0, securityCurrentUp: 0, securityTempLow: 0, securityTempUp: 0,
            securityCapUp: 0, securityDelay: 0, protectDetalVolTime: 0, protectDetalVol: 0
        };

        if (!this.checkParam(this.ui.boxRecordVoltGap, this.ui.lineRecordVoltGap, vars, "recordVoltGap", 0, 100000)
            || !this.checkParam(this.ui.boxRecordCurrentGap, this.ui.lineRecordCurrentGap, vars, "recordCurrentGap", 0, 100000)
            || !this.checkParam(this.ui.boxSecurityVoltLow, this.ui.lineSecurityVoltLow, vars, "securityVoltLow", 0, maxVoltage)
            || !this.checkParam(this.ui.boxSecurityVoltUp, this.ui.lineSecurityVoltUp, vars, "securityVoltUp", vars.securityVoltLow, maxVoltage)
            || !this.checkParam(this.ui.boxSecurityCurrentLow, this.ui.lineSecurityCurrentLow, vars, "securityCurrentLow", 0, maxCurrent)
            || !this.checkParam(this.ui.boxSecurityCurrentUp, this.ui.lineSecurityCurrentUp, vars, "securityCurrentUp", vars.securityCurrentLow, maxCurrent)
            || !this.checkParam(this.ui.boxSecurityTempLow, this.ui.lineSecurityTempLow, vars, "securityTempLow", 0, maxTemp)
            || !this.checkParam(this.ui.boxSecurityTempUp, this.ui.lineSecurityTempUp, vars, "securityTempUp", vars.securityTempLow, maxTemp)
            || !this.checkParam(this.ui.boxSecurityDelay, this.ui.lineSecurityDelay, vars, "securityDelay", 0, maxDelay)
            || !this.checkParam(this.ui.boxProtectDetal, this.ui.lineProtectDetalTime, vars, "protectDetalVolTime", 0, maxDelay, false)
            || !this.checkParam(this.ui.boxProtectDetal, this.ui.lineProtectDetalVol, vars, "protectDetalVol", 0, maxDelay, false))
            return false;

        const extParam = [];
        extParam.push(PARAM_RecordTimeInterval, PARAM_RecordVoltageChange, PARAM_RecordCurrentChange,
            PARAM_ProtectVoltageLower, PARAM_ProtectVoltageUpper,
            PARAM_ProtectCurrentLower, PARAM_ProtectCurrentUpper, PARAM_ProtectTempatureLower,
            PARAM_ProtectTempatureUpper, PARAM_ProtectDelayTime,
            PARAM_ProtectDetalVolTime, PARAM_ProtectDetalVol);

        const list = this.stepInfo.step.stepList;
        for (let i = 0; i < list.length; i++)
        {
            const step = list[i];
            for (let j = 0; j < extParam.length; j++)
                step.paramList[extParam[j]].clear();

            if (step.StepType === STEP_TYPE_STOP)
                continue;

            if (this.ui.boxRecordTimeGap.isChecked())
            {
                const sec = parseInt(this.ui.lineRecordTimeGap.text(), 10);
                //结束和循环操作不需要
                step.setRecordTimeInterval(OP_BIG_EQU, sec * 1000);
            }

            //电压间隔
            if (this.ui.boxRecordVoltGap.isChecked())
            {
                step.setRecordVoltageChange(OP_BIG_EQU, vars.recordVoltGap);
            }

            //电流间隔
            if (this.ui.boxRecordCurrentGap.isChecked())
            {
                step.setRecordCurrentChange(OP_BIG_EQU, vars.recordCurrentGap);
            }

            //电压下限
            if (this.ui.boxSecurityVoltLow.isChecked())
            {
                step.setProtectVoltageLower(OP_BIG_EQU, vars.securityVoltLow);
            }
            //电压上限
            if (this.ui.boxSecurityVoltUp.isChecked())
            {
                step.setProtectVoltageUpper(OP_SMALL_EQU, vars.securityVoltUp);
            }
            //电流下限
            if (this.ui.boxSecurityCurrentLow.isChecked())
            {
                step.setProtectCurrentLower(OP_BIG_EQU, vars.securityCurrentLow);
            }
            //电流上限
            if (this.ui.boxSecurityCurrentUp.isChecked())
            {
                step.setProtectCurrentUpper(OP_SMALL_EQU, vars.securityCurrentUp);
            }
            //温度下限
            if (this.ui.boxSecurityTempLow.isChecked())
            {
                step.setProtectTempatureLower(OP_BIG_EQU, vars.securityTempLow);
            }
            //温度上限
            if (this.ui.boxSecurityTempUp.isChecked())
            {
                step.setProtectTempatureUpper(OP_SMALL_EQU, vars.securityTempUp);
            }
            //延迟时间
            if (this.ui.boxSecurityDelay.isChecked())
            {
                step.setProtectDelayTime(OP_BIG_EQU, vars.securityDelay * 1000);
            }
            //化成变化率
            if (this.ui.boxProtectDetal.isChecked())
            {
                step.setProtectDetalVol(OP_BIG_EQU, vars.protectDetalVol);
                step.setProtectDetalVolTime(OP_BIG_EQU, vars.protectDetalVolTime * 1000);
            }
        }

        //更新排序
        this.stepInfo.step.updateStepLine();

        return true;
    }

    // void updateTable();
    updateTable()
    {
        this.m_tableHelper.updateTable(this.stepInfo);
    }

    // void on_btnAdd_clicked();
    on_btnAdd_clicked()
    {
        if (this.stepInfo.step.stepList.length >= 13)
        {
            JZMessageBox.information(null, "", tr("工步数量不能超出13个"));
            return;
        }

        const dlg = new StepEditDialog(this);
        dlg.DeviceString = this.DeviceString;
        dlg.itemIndex = this.itemIndex;
        dlg.step.StepType = STEP_TYPE_7;
        dlg.step.StepLine = this.ui.tableWidget.rowCount();
        if (dlg.exec() !== QDialog.Accepted)
            return;

        const idx = this.stepInfo.step.stepList.length - 1;
        this.stepInfo.step.stepList.splice(idx, 0, dlg.step);   // stepList.insert(idx, dlg.step)

        this.ui.tableWidget.setCurrentCell(this.ui.tableWidget.rowCount() - 1, 0);
        this.updateTable();
    }

    // void on_btnDel_clicked();
    on_btnDel_clicked()
    {
        const row = this.ui.tableWidget.currentRow();
        if (row < 0)
            return;
        if (row === this.ui.tableWidget.rowCount() - 1)
        {
            JZMessageBox.information(null, "", tr("结束操作不能删除"));
            return;
        }

        this.stepInfo.step.stepList.splice(row, 1);   // stepList.remove(row)
        if (row < this.ui.tableWidget.currentRow())
            this.ui.tableWidget.setCurrentCell(row, 0);

        this.updateTable();
    }

    // void on_btnEdit_clicked();
    on_btnEdit_clicked()
    {
        const row = this.ui.tableWidget.currentRow();
        if (row < 0)
            return;
        if (row === this.ui.tableWidget.rowCount() - 1)
        {
            JZMessageBox.information(null, "", tr("结束操作不能修改"));
            return;
        }

        const dlg = new StepEditDialog(this);
        dlg.DeviceString = this.DeviceString;
        dlg.itemIndex = this.itemIndex;
        dlg.step = this.stepInfo.step.stepList[row];
        dlg.step.StepLine = row + 1;
        if (dlg.exec() !== QDialog.Accepted)
            return;

        this.stepInfo.step.stepList[row] = dlg.step;
        this.updateTable();
    }

    // void on_btnUp_clicked();
    on_btnUp_clicked()
    {
        const row = this.ui.tableWidget.currentRow();
        if (row > 0 && row !== this.ui.tableWidget.rowCount() - 1)
        {
            swap(this.stepInfo.step.stepList, row, row - 1);   // qSwap(stepList[row], stepList[row - 1])

            this.ui.tableWidget.setCurrentCell(row - 1, 0);
            this.updateTable();
        }
    }

    // void on_btnDown_clicked();
    on_btnDown_clicked()
    {
        const row = this.ui.tableWidget.currentRow();
        if (row >= 0 && row < this.ui.tableWidget.rowCount() - 2)
        {
            swap(this.stepInfo.step.stepList, row, row + 1);   // qSwap(stepList[row], stepList[row + 1])

            this.ui.tableWidget.setCurrentCell(row + 1, 0);
            this.updateTable();
        }
    }

    // void on_btnInit_clicked();
    on_btnInit_clicked()
    {
        // 原: question(..., Yes|No) != Yes —— JZMessageBox.question 是 OK/Cancel 两键, 1 = OK
        if (JZMessageBox.question(null, "", tr("警告，重置将会清除所有数据")) !== 1)
            return;

        this.stepInfo.step.genDefaultStep();
        this.initStep();
    }

    // void on_btnSave_clicked();
    on_btnSave_clicked()
    {
        if (!this.uiToStep())
            return;

        if (!this.checkStep())
            return;

        // 引擎的文件对话框: 取消返回 null(原来 QFileDialog 返回空串)
        const filename = JZFileDialog.getSaveFileName(null, "", "StepInfo.step", "*.step");
        if (!filename)
            return;

        // 原: QFile + QDataStream s << stepInfo;  —— 工步文件格式自由, 改用 JSON 落盘
        fs.writeFileSync(filename, JSON.stringify(btStepInfoToJson(this.stepInfo), null, 2), "utf8");
    }

    // void on_btnOpen_clicked();
    on_btnOpen_clicked()
    {
        // 引擎的文件对话框: 取消返回 null(原来 QFileDialog 返回空串)
        const filename = JZFileDialog.getOpenFileName(null, "", "", "*.step");
        if (!filename)
            return;

        // 原: QFile + QDataStream s >> stepInfo;  —— 现在是 JSON 文本
        const j = JSON.parse(fs.readFileSync(filename, "utf8"));
        this.stepInfo = new BTEStepInfo();
        btStepInfoSetJson(this.stepInfo, j);

        this.initStep();
    }

    // void on_btnBack_clicked();
    on_btnBack_clicked()
    {

    }

    // void on_btnSet_clicked();
    on_btnSet_clicked()
    {

    }

    // bool checkParam(QCheckBox *box, QLineEdit *edit, double &var, double low, double upper, bool must = true);
    //   出参 double& var -> 写入 vars[key]
    checkParam(box, edit, vars, key, low, upper, must = true)
    {
        if (box.isChecked())
        {
            const title = box.text().trim();
            if (!must && title === "")
                return true;

            let ok = true;
            const text = edit.text();
            const var_ = Number(text);
            ok = (text.trim() !== "") && !isNaN(var_);
            vars[key] = ok ? var_ : 0;
            if (!ok)
            {
                JZMessageBox.information(null, "", title + tr("格式错误，请检查"));
            }
            else if (var_ < low || var_ > upper)
            {
                JZMessageBox.information(null, "", title + sprintf(tr("取值范围错误，应为:%0.2f - %0.2f"), low, upper));
                ok = false;
            }
            return ok;
        }
        return true;
    }

    // bool checkStep();
    checkStep()
    {
        const scale = g_project.deviceData(this.DeviceString).scale(this.itemIndex);
        const volt_max = g_project.deviceData(this.DeviceString).maxVoltage() * scale;
        const current_max = g_project.deviceData(this.DeviceString).maxCurrent() * scale;

        for (let i = 0; i < this.stepInfo.step.stepList.length; i++)
        {
            const step = this.stepInfo.step.stepList[i];
            const pre = tr("工步") + String(i + 1);
            if (step.isVoltageVaild() && step.Voltage() > volt_max)
            {
                JZMessageBox.information(null, "", pre + sprintf(tr("电压应小于%0.2fV"), volt_max));
                return false;
            }
            if (step.isStopVoltageVaild() && step.StopVoltage() > volt_max)
            {
                JZMessageBox.information(null, "", pre + sprintf(tr("截止电压应小于%0.2fV"), volt_max));
                return false;
            }
            if (step.isCurrentVaild() && step.Current() > current_max)
            {
                JZMessageBox.information(null, "", pre + sprintf(tr("电流应小于%0.2fA"), current_max));
                return false;
            }
            if (step.isStopCurrentVaild() && step.StopCurrent() > current_max)
            {
                JZMessageBox.information(null, "", pre + sprintf(tr("截止电流应小于%0.2fA"), current_max));
                return false;
            }
            if (step.isCapacityVaild() && step.Capacity() > MAX_STEP_CAPACITY)
            {
                JZMessageBox.information(null, "", pre + sprintf(tr("容量应小于%0.2fAh"), MAX_STEP_CAPACITY));
                return false;
            }
            if (step.isTempatureVaild() && step.Tempature() > MAX_STEP_TEMPATURE)
            {
                JZMessageBox.information(null, "", pre + sprintf(tr("最大温度应小于%0.1f℃"), MAX_STEP_TEMPATURE));
                return false;
            }
        }

        return true;
    }

    // void on_btnOk_clicked();
    on_btnOk_clicked()
    {
        if (!this.uiToStep())
            return;

        if (!this.checkStep())
            return;

        this.accept();
    }

    // void on_btnCancel_clicked();
    on_btnCancel_clicked()
    {
        this.reject();
    }

    // void on_tableWidget_itemPressed(QTableWidgetItem *item);
    on_tableWidget_itemPressed(item)
    {
    }

    // void on_tableWidget_itemDoubleClicked(QTableWidgetItem *item);
    on_tableWidget_itemDoubleClicked(item)
    {
        if (item.row() >= 0)
            this.on_btnEdit_clicked();
    }
}

// qSwap(a, b) 的等价实现
function swap(list, i, j)
{
    const t = list[i];
    list[i] = list[j];
    list[j] = t;
}
