// ---------------------------------------------------------------------------
// StepInfoDialog.js —— 对应 C++ BTEServer/StepInfoDialog.h + StepInfoDialog.cpp
//
// 通道信息对话框: 左半边是采集信息/保护上下限表格, 右半边是工步表(StepTableHelper)。
// ---------------------------------------------------------------------------

import { tr, sprintf } from './BTEServer.js';
import { UiHelper } from './Common.js';
import { formatDateTime } from './datetime.js';
import { StepTableHelper } from './StepSettingDialog.js';

// ---------------------------------------------------------------------------
// class StepInfoDialog : public QDialog
// ---------------------------------------------------------------------------
export class StepInfoDialog
{
    // StepInfoDialog(QWidget *parent = Q_NULLPTR);
    constructor(parent)
    {
        QDialog.call(this, parent);   // : QDialog(parent)

        this.record = null;   // BTERecordPtr record;

        this.ui = new Ui_StepInfoDialogClass();
        this.ui.setupUi(this);

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
    showEvent(event)
    {
        this.updateTable();
        QDialog.prototype.showEvent.call(this, event);   // __super::showEvent(event);
    }

    // void updateTable();
    updateTable()
    {
        const step = this.record.info.stepInfo;

        //info
        this.ui.tableInfo.setRowCount(7);

        this.ui.tableInfo.setItem(0, 0, new QTableWidgetItem(tr("设备_单元_通道")));
        this.ui.tableInfo.setItem(0, 1, new QTableWidgetItem(sprintf("%d-%02d", (this.record.info.itemIndex / 8 | 0) + 1, this.record.info.itemIndex % 8 + 1)));

        this.ui.tableInfo.setItem(1, 0, new QTableWidgetItem(tr("启动时间")));
        this.ui.tableInfo.setItem(1, 1, new QTableWidgetItem(formatDateTime(this.record.info.recordTime, "yyyy-MM-dd HH:mm:ss")));

        this.ui.tableInfo.setItem(2, 0, new QTableWidgetItem(tr("起始工步")));
        this.ui.tableInfo.setItem(2, 1, new QTableWidgetItem(String(step.startStepIndex)));

        this.ui.tableInfo.setItem(3, 0, new QTableWidgetItem(tr("记录条件-时间")));
        this.ui.tableInfo.setItem(4, 0, new QTableWidgetItem(tr("记录条件-电压")));
        this.ui.tableInfo.setItem(5, 0, new QTableWidgetItem(tr("记录条件-电流")));

        this.ui.tableInfo.setItem(0, 2, new QTableWidgetItem(tr("电压下限")));
        this.ui.tableInfo.setItem(1, 2, new QTableWidgetItem(tr("电压上限")));
        this.ui.tableInfo.setItem(2, 2, new QTableWidgetItem(tr("电流下限")));
        this.ui.tableInfo.setItem(3, 2, new QTableWidgetItem(tr("电流上限")));
        this.ui.tableInfo.setItem(4, 2, new QTableWidgetItem(tr("温度上限")));
        this.ui.tableInfo.setItem(5, 2, new QTableWidgetItem(tr("温度下限")));
        this.ui.tableInfo.setItem(6, 2, new QTableWidgetItem(tr("延迟时间")));

        this.ui.tableInfo.setItem(0, 4, new QTableWidgetItem(tr("化成检测时间")));
        this.ui.tableInfo.setItem(1, 4, new QTableWidgetItem(tr("化成电压值")));

        if (step.step.stepList.length > 0)
        {
            const stepInfo = step.step.stepList[0];
            //记录
            if (stepInfo.isRecordTimeIntervalVaild())
                this.ui.tableInfo.setItem(3, 1, new QTableWidgetItem(String((stepInfo.RecordTimeInterval() / 1000) | 0) + "s"));
            if (stepInfo.isRecordVoltageChangeVaild())
                this.ui.tableInfo.setItem(4, 1, new QTableWidgetItem(String(stepInfo.RecordVoltageChange()) + "V"));
            if (stepInfo.isRecordCurrentChangeVaild())
                this.ui.tableInfo.setItem(5, 1, new QTableWidgetItem(String(stepInfo.RecordCurrentChange()) + "A"));

            //上下限
            if (stepInfo.isProtectVoltageLowerVaild())
                this.ui.tableInfo.setItem(0, 3, new QTableWidgetItem(Number(stepInfo.ProtectVoltageLower()).toFixed(4) + "V"));

            if (stepInfo.isProtectVoltageUpperVaild())
                this.ui.tableInfo.setItem(1, 3, new QTableWidgetItem(Number(stepInfo.ProtectVoltageUpper()).toFixed(4) + "V"));

            if (stepInfo.isProtectCurrentLowerVaild())
                this.ui.tableInfo.setItem(2, 3, new QTableWidgetItem(Number(stepInfo.ProtectCurrentLower()).toFixed(4) + "A"));

            if (stepInfo.isProtectCurrentUpperVaild())
                this.ui.tableInfo.setItem(3, 3, new QTableWidgetItem(Number(stepInfo.ProtectCurrentUpper()).toFixed(4) + "A"));

            if (stepInfo.isProtectTempatureLowerVaild())
                this.ui.tableInfo.setItem(4, 3, new QTableWidgetItem(Number(stepInfo.ProtectTempatureLower()).toFixed(4) + "°C"));

            if (stepInfo.isProtectTempatureUpperVaild())
                this.ui.tableInfo.setItem(5, 3, new QTableWidgetItem(Number(stepInfo.ProtectTempatureUpper()).toFixed(4) + "°C"));

            if (stepInfo.isProtectDelayTimeVaild())
                this.ui.tableInfo.setItem(6, 3, new QTableWidgetItem(String((stepInfo.ProtectDelayTime() / 1000) | 0) + "S"));

            if (stepInfo.isProtectDetalVolVaild())
                this.ui.tableInfo.setItem(0, 5, new QTableWidgetItem(Number(stepInfo.ProtectDetalVol()).toFixed(4) + "V"));

            if (stepInfo.isProtectDetalVolTimeVaild())
                this.ui.tableInfo.setItem(1, 5, new QTableWidgetItem(String((stepInfo.ProtectDetalVolTime() / 1000) | 0) + "S"));
        }


        this.ui.tableInfo.setItem(2, 4, new QTableWidgetItem(tr("电池批号")));
        this.ui.tableInfo.setItem(2, 5, new QTableWidgetItem(step.cellBatch));

        this.ui.tableInfo.setItem(3, 4, new QTableWidgetItem(tr("电池编号")));
        this.ui.tableInfo.setItem(3, 5, new QTableWidgetItem(step.cellNo));

        this.ui.tableInfo.setItem(4, 4, new QTableWidgetItem(tr("创建人")));
        this.ui.tableInfo.setItem(4, 5, new QTableWidgetItem(step.creator));

        this.ui.tableInfo.setItem(5, 4, new QTableWidgetItem(tr("备注")));
        this.ui.tableInfo.setItem(6, 5, new QTableWidgetItem(step.memo));

        //info
        this.m_tableHelper.updateTable(step);
    }
}
