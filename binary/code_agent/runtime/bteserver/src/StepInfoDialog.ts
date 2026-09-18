// ---------------------------------------------------------------------------
// StepInfoDialog.js —— 对应 C++ BTEServer/StepInfoDialog.h + StepInfoDialog.cpp
//
// 通道信息对话框: 左半边是采集信息/保护上下限表格, 右半边是工步表(StepTableHelper)。
// ---------------------------------------------------------------------------

import { tr, sprintf } from './BTEServer.js';
import { UiHelper } from './Common.js';
import { formatDateTime } from './datetime.js';
import { StepTableHelper } from './StepSettingDialog.js';
import { STEPINFODIALOG_UI_XML } from './ui/STEPINFODIALOG_UI_XML.js';

// ---------------------------------------------------------------------------
// class StepInfoDialog : public QDialog
// ---------------------------------------------------------------------------
export class StepInfoDialog
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // StepInfoDialog(QWidget *parent = Q_NULLPTR);
    constructor(parent)
    {
        this.record = null;   // BTERecordPtr record;

        // 真 UI: 原版 StepInfoDialog.ui 转换出的 XML; root 是**新窗口**(JZDialog), 不是 this。
        // (Qt 的 `QDialog.call(this, parent)` 那套继承不再需要 —— 本类改成"持一个真窗口"。)
        this.ui = uiLoader.load(STEPINFODIALOG_UI_XML);
        // parent 传进来的是"持真窗口的 JS 对象"(如 BTEServer), 真控件在它的 .ui 上; 取对了再挂。
        if (parent) { const pw = parent.ui ? parent.ui : parent; if (pw) this.ui.setParentWidget(pw); }

        const strListHeader = [];
        strListHeader.push(tr("通道属性"), tr("通道状态"), tr("通道属性"), tr("通道状态"), tr("通道属性"),
            tr("通道状态"));

        UiHelper.SetTable(this.ui.tableInfo, strListHeader);
        this.m_tableHelper = new StepTableHelper();
        this.m_tableHelper.init(this.ui.tableStep);
    }

    // ~StepInfoDialog();
    dispose()
    {
    }

    // void showEvent(QShowEvent *event);
    onShow(event)
    {
        this.updateTable();
    }

    // void updateTable();
    updateTable()
    {
        const step = this.record.info.stepInfo;

        //info
        this.ui.tableInfo.setRowCount(7);

        this.ui.tableInfo.setItemText(0, 0, tr("设备_单元_通道"));
        this.ui.tableInfo.setItemText(0, 1, sprintf("%d-%02d", (this.record.info.itemIndex / 8 | 0) + 1, this.record.info.itemIndex % 8 + 1));

        this.ui.tableInfo.setItemText(1, 0, tr("启动时间"));
        this.ui.tableInfo.setItemText(1, 1, formatDateTime(this.record.info.recordTime, "yyyy-MM-dd HH:mm:ss"));

        this.ui.tableInfo.setItemText(2, 0, tr("起始工步"));
        this.ui.tableInfo.setItemText(2, 1, String(step.startStepIndex));

        this.ui.tableInfo.setItemText(3, 0, tr("记录条件-时间"));
        this.ui.tableInfo.setItemText(4, 0, tr("记录条件-电压"));
        this.ui.tableInfo.setItemText(5, 0, tr("记录条件-电流"));

        this.ui.tableInfo.setItemText(0, 2, tr("电压下限"));
        this.ui.tableInfo.setItemText(1, 2, tr("电压上限"));
        this.ui.tableInfo.setItemText(2, 2, tr("电流下限"));
        this.ui.tableInfo.setItemText(3, 2, tr("电流上限"));
        this.ui.tableInfo.setItemText(4, 2, tr("温度上限"));
        this.ui.tableInfo.setItemText(5, 2, tr("温度下限"));
        this.ui.tableInfo.setItemText(6, 2, tr("延迟时间"));

        this.ui.tableInfo.setItemText(0, 4, tr("化成检测时间"));
        this.ui.tableInfo.setItemText(1, 4, tr("化成电压值"));

        if (step.step.stepList.length > 0)
        {
            const stepInfo = step.step.stepList[0];
            //记录
            if (stepInfo.isRecordTimeIntervalVaild())
                this.ui.tableInfo.setItemText(3, 1, String((stepInfo.RecordTimeInterval() / 1000) | 0) + "s");
            if (stepInfo.isRecordVoltageChangeVaild())
                this.ui.tableInfo.setItemText(4, 1, String(stepInfo.RecordVoltageChange()) + "V");
            if (stepInfo.isRecordCurrentChangeVaild())
                this.ui.tableInfo.setItemText(5, 1, String(stepInfo.RecordCurrentChange()) + "A");

            //上下限
            if (stepInfo.isProtectVoltageLowerVaild())
                this.ui.tableInfo.setItemText(0, 3, Number(stepInfo.ProtectVoltageLower()).toFixed(4) + "V");

            if (stepInfo.isProtectVoltageUpperVaild())
                this.ui.tableInfo.setItemText(1, 3, Number(stepInfo.ProtectVoltageUpper()).toFixed(4) + "V");

            if (stepInfo.isProtectCurrentLowerVaild())
                this.ui.tableInfo.setItemText(2, 3, Number(stepInfo.ProtectCurrentLower()).toFixed(4) + "A");

            if (stepInfo.isProtectCurrentUpperVaild())
                this.ui.tableInfo.setItemText(3, 3, Number(stepInfo.ProtectCurrentUpper()).toFixed(4) + "A");

            if (stepInfo.isProtectTempatureLowerVaild())
                this.ui.tableInfo.setItemText(4, 3, Number(stepInfo.ProtectTempatureLower()).toFixed(4) + "°C");

            if (stepInfo.isProtectTempatureUpperVaild())
                this.ui.tableInfo.setItemText(5, 3, Number(stepInfo.ProtectTempatureUpper()).toFixed(4) + "°C");

            if (stepInfo.isProtectDelayTimeVaild())
                this.ui.tableInfo.setItemText(6, 3, String((stepInfo.ProtectDelayTime() / 1000) | 0) + "S");

            if (stepInfo.isProtectDetalVolVaild())
                this.ui.tableInfo.setItemText(0, 5, Number(stepInfo.ProtectDetalVol()).toFixed(4) + "V");

            if (stepInfo.isProtectDetalVolTimeVaild())
                this.ui.tableInfo.setItemText(1, 5, String((stepInfo.ProtectDetalVolTime() / 1000) | 0) + "S");
        }


        this.ui.tableInfo.setItemText(2, 4, tr("电池批号"));
        this.ui.tableInfo.setItemText(2, 5, step.cellBatch);

        this.ui.tableInfo.setItemText(3, 4, tr("电池编号"));
        this.ui.tableInfo.setItemText(3, 5, step.cellNo);

        this.ui.tableInfo.setItemText(4, 4, tr("创建人"));
        this.ui.tableInfo.setItemText(4, 5, step.creator);

        this.ui.tableInfo.setItemText(5, 4, tr("备注"));
        this.ui.tableInfo.setItemText(6, 5, step.memo);

        //info
        this.m_tableHelper.updateTable(step);
    }
}
