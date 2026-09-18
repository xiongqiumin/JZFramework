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
import { SPLITDIALOG_UI_XML } from './ui/SPLITDIALOG_UI_XML.js';

// ---------------------------------------------------------------------------
// class SplitDialog : public QDialog
// ---------------------------------------------------------------------------
export class SplitDialog
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // SplitDialog(QWidget *parent = Q_NULLPTR);
    constructor(parent)
    {
        // 原 `QDialog.call(this, parent)`(: QDialog(parent)) 已去掉 —— 本类不再"继承 Qt 对话框",
        // 而是**持一个真窗口**(this.ui, 见下), 窗口方法都转发到它。

        this.recordList = [];   // QVector<BTERecordPtr> recordList;

        // 真 UI: 原版 SplitDialog.ui 转换出的 XML; root 是**新窗口**(JZDialog), 不是 this。
        this.ui = uiLoader.load(SPLITDIALOG_UI_XML);
        // parent 传进来的是"持真窗口的 JS 对象"(如 BTEServer), 真控件在它的 .ui 上; 取对了再挂。
        if (parent) { const pw = parent.ui ? parent.ui : parent; if (pw) this.ui.setParentWidget(pw); }

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
        // 原 Qt 的 horizontalHeader().setSectionResizeMode(Interactive)(可拖列宽)在 JZTableWidget
        // 上没有对应的表头对象/接口 —— 去掉(要开放列宽得先给表格/绑定加 setColumnWidth)。
    }

    // ~SplitDialog();
    dispose()
    {
    }

    // void showEvent(QShowEvent *event);
    onShow(event)
    {
        this.ui.splitSetting.init();
        this.ui.splitSetting.recordList = this.recordList;
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

                // 值级接口: 第 0 列 = 等级, 第 1 列 = 电池个数。
                // 原 Qt 版第 0 列是 QCheckBox(等级勾选)—— JZ 表格**没有单元格控件**, 也没有点击信号,
                // 勾选交互没法替换 ⇒ 去掉该列, 分选按**全部等级**处理(见 onSplitItemNo)。
                this.ui.tableSplitLevel.setItemText(row, 0, name);
                this.ui.tableSplitLevel.setItemText(row, 1, String(split_ret.indexs[i].length));

                row++;
            }
        }
        this.ui.tableSplitLevel.setRowCount(row);
        this.onSplitItemNo();
    }

    // void onSplitItemNo();
    //   原 Qt 版按"等级勾选框"挑通道; JZ 表格没有勾选交互 ⇒ 全部等级都算。
    onSplitItemNo()
    {
        const split_ret = this.ui.splitSetting.result;

        const items = [];
        for (let level = 0; level < split_ret.indexs.length; level++)
            items.push(...split_ret.indexs[level]);   // items << split_ret.indexs[level];

        //update table
        this.ui.tableSplitList.clearContents();
        this.ui.tableSplitList.setRowCount(items.length);
        for (let i = 0; i < items.length; i++)
        {
            const rec_idx = items[i];
            const info = split_ret.recordList[rec_idx];
            const item_idx = info.itemIndex;

            this.ui.tableSplitList.setItemText(i, 0, String(i + 1));
            this.ui.tableSplitList.setItemText(i, 1, String((item_idx / 8 | 0) + 1) + "-" + String(item_idx % 8 + 1));
            this.ui.tableSplitList.setItemText(i, 2, String(info.DisChargeCap));
            this.ui.tableSplitList.setItemText(i, 3, info.TimeText());
            this.ui.tableSplitList.setItemText(i, 4, String(info.MeanVoltage));
            this.ui.tableSplitList.setItemText(i, 5, Number(info.CcRatio).toFixed(2));
            this.ui.tableSplitList.setItemText(i, 6, info.LevelText());
            this.ui.tableSplitList.setItemText(i, 7, info.CellNo);
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
                line.push(this.ui.tableSplitList.itemText(i, j));
            }
            text.push(line);
        }
        ExportSplitResult(this, text);
    }
}
