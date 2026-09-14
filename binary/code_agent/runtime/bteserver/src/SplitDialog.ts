// ---------------------------------------------------------------------------
// SplitDialog.js —— 对应 C++ BTEServer/SplitDialog.h + SplitDialog.cpp
//
// 查询页"分选"按钮弹出的对话框: 内嵌 SplitSettingWidget + 饼图 + 等级/明细两张表。
// (C++ 里同文件的自由函数 ExportSplitResult 定义在 SplitSettingWidget.js 中导出)
//
// Qt -> JS: 槽里的 sender()->property("level") -> 由信号回调把控件传进槽。
// ---------------------------------------------------------------------------

import { tr, sprintf } from './BTEServer.js';
import { UiHelper } from './Common.js';
import { SplitSettingWidget, ExportSplitResult } from './SplitSettingWidget.js';

// ---------------------------------------------------------------------------
// class SplitDialog : public QDialog
// ---------------------------------------------------------------------------
export class SplitDialog
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // SplitDialog(QWidget *parent = Q_NULLPTR);
    constructor(parent)
    {
        QDialog.call(this, parent);   // : QDialog(parent)

        this.recordList = [];   // QVector<BTERecordPtr> recordList;

        this.ui = new Ui_SplitDialogClass();
        this.ui.setupUi(this);

        this.ui.splitSetting.sigSplitFinish.push(() => this.onSplitFinish());   // connect(ui.splitSetting, SIGNAL(sigSplitFinish()), this, SLOT(onSplitFinish()));

        //tableSplitLevel
        const strListSplitLevel = [];
        strListSplitLevel.push(tr("级别"), tr("电池个数"));
        UiHelper.SetTable(this.ui.tableSplitLevel, strListSplitLevel);

        //tableSplitList
        const strListSplitList = [];
        strListSplitList.push(tr("序号"), tr("设备-单元-通道"), tr("放电容量(Ah)"), tr("放电时间（H:M:S:MS）"),
            tr("中值电压（V）"), tr("恒流比（%）"), tr("级别"), tr("电池批号"));
        UiHelper.SetTable(this.ui.tableSplitList, strListSplitList);
        this.ui.tableSplitList.horizontalHeader().setSectionResizeMode(QHeaderView.Interactive);
    }

    // ~SplitDialog();
    dispose()
    {
    }

    // void showEvent(QShowEvent *event);
    showEvent(event)
    {
        this.ui.splitSetting.init();
        this.ui.splitSetting.recordList = this.recordList;
        QDialog.prototype.showEvent.call(this, event);   // __super::showEvent(event);
    }

    // void onSplitFinish();
    onSplitFinish()
    {
        const split_ret = this.ui.splitSetting.result;
        if (split_ret.error)
        {
            const tips = sprintf(tr("分选失败 第%d条记录,无有效放电数据"), split_ret.erroNum);
            // parent 传 null: 本类还不是真控件(见文件头), 引擎的弹窗只收真 JZWidget 当宿主
            JZMessageBox.information(null, "", tips);
            return;
        }
        this.ui.splitPie.setResult(split_ret);

        //总览
        const range = this.ui.splitSetting.range();
        this.ui.tableSplitLevel.clearContents();
        this.ui.tableSplitLevel.setRowCount(16);
        let row = 0;
        for (let i = 0; i < 16; i++)
        {
            if (i === 15 || range[i].vaild)
            {
                const name = (i < 15) ? String.fromCharCode('A'.charCodeAt(0) + i) : tr("落选");

                const itemNumber = new QTableWidgetItem(String(split_ret.indexs[i].length));

                const itemNo = new QCheckBox(name);
                itemNo.setProperty("level", i);
                itemNo.setChecked(true);
                // C++ 槽里用 sender() 取控件, JS 侧由回调把控件传进去
                itemNo.onClicked = () => this.onSplitItemNo(itemNo);

                this.ui.tableSplitLevel.setCellWidget(row, 0, itemNo);
                this.ui.tableSplitLevel.setItem(row, 1, itemNumber);

                row++;
            }
        }
        this.ui.tableSplitLevel.setRowCount(row);
        this.onSplitItemNo();
    }

    // void onSplitItemNo();
    //   C++ 里 idx 来自 sender()->property("level"); JS 侧由信号回调传入控件。
    onSplitItemNo(senderWidget)
    {
        const split_ret = this.ui.splitSetting.result;

        const idx = Number(senderWidget.property("level"));
        const items = [];
        const count = this.ui.tableSplitLevel.rowCount();
        for (let i = 0; i < count; i++)
        {
            const box = this.ui.tableSplitLevel.cellWidget(i, 0);   // (QCheckBox*)
            const level = Number(box.property("level"));
            if (box.isChecked())
                items.push(...split_ret.indexs[level]);   // items << split_ret.indexs[level];
        }

        //update table
        this.ui.tableSplitList.clearContents();
        this.ui.tableSplitList.setRowCount(items.length);
        for (let i = 0; i < items.length; i++)
        {
            const rec_idx = items[i];
            const info = split_ret.recordList[rec_idx];
            const item_idx = info.itemIndex;

            const itemNo = new QTableWidgetItem(String(i + 1));
            const itemPos = new QTableWidgetItem(String((item_idx / 8 | 0) + 1) + "-" + String(item_idx % 8 + 1));
            const itemCap = new QTableWidgetItem(String(info.DisChargeCap));
            const itemTime = new QTableWidgetItem(info.TimeText());
            const itemVolt = new QTableWidgetItem(String(info.MeanVoltage));
            const itemRatio = new QTableWidgetItem(Number(info.CcRatio).toFixed(2));
            const itemLevel = new QTableWidgetItem(info.LevelText());
            const itemCellNo = new QTableWidgetItem(info.CellNo);

            this.ui.tableSplitList.setItem(i, 0, itemNo);
            this.ui.tableSplitList.setItem(i, 1, itemPos);
            this.ui.tableSplitList.setItem(i, 2, itemCap);
            this.ui.tableSplitList.setItem(i, 3, itemTime);
            this.ui.tableSplitList.setItem(i, 4, itemVolt);
            this.ui.tableSplitList.setItem(i, 5, itemRatio);
            this.ui.tableSplitList.setItem(i, 6, itemLevel);
            this.ui.tableSplitList.setItem(i, 7, itemCellNo);
        }
    }

    // void on_btnSaveCopy_clicked();
    on_btnSaveCopy_clicked()
    {
        const count = this.ui.tableSplitList.rowCount();
        const text = [];   // QVector<QStringList> text
        const head = [];
        head.push(tr("序号"), tr("设备-单元-通道"), tr("放电容量(Ah)"), tr("放电时间（H:M:S:MS）"),
            tr("中值电压（V）"), tr("恒流比（%）"), tr("级别"), tr("电池批号"));

        text.push(head);
        for (let i = 0; i < count; i++)
        {
            const line = [];
            for (let j = 0; j < 8; j++)
            {
                line.push(this.ui.tableSplitList.item(i, j).text());
            }
            text.push(line);
        }
        ExportSplitResult(this, text);
    }
}
