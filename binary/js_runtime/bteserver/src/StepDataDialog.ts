// ---------------------------------------------------------------------------
// StepDataDialog.js —— 对应 C++ BTEServer/StepDataDialog.h + StepDataDialog.cpp
//
// 通道数据窗口: 三张 qcustomplot 曲线(电流/电压、容量/电压、容量/比容量) + 工步树。
//
// 说明: qcustomplot(与 nightcharts / libxlsx / sqlite3 一样)是随工程分发的第三方 Qt 库,
//       不参与 JS 移植, 调用点按 Qt API 原样保留, 由运行时提供等价实现。
// Qt -> JS: QVector::resize+fill -> new Array(n).fill(v); std::equal_range(中值查找)
//           -> findIndex(v >= var); 信号 -> 回调数组 + emit()。
// ---------------------------------------------------------------------------

import { tr, emit, sprintf } from './BTEServer.js';
import { stru_BTS_CMD_SEND_DFDATA76, aTimeToMSec, aTimeToISO } from './structs.js';
import { PackDefine, STEP_CHARGE, STEP_CHARGE_OTHER, STEP_DISCHARGE, STEP_TYPE_NULL, STEP_TYPE_STOP } from './PackDefine.js';
import { DataTrans } from './DataTrans.js';
import { loadUiFile } from './uiFile.js';

// Qt 枚举/具名颜色里本文件用到的那几项(值与原 Qt 一致): 原来由 qtcompat 提供。
// 注: qcustomplot 是第三方 Qt 库(不参与 JS 移植), 这里的画笔/角色值只是喂给它的替身。
const Qt = {
    UserRole: 0x0100,
    blue: '#0000ff',
    red: '#ff0000',
};

// ---------------------------------------------------------------------------
// class StepDataDialog : public QDialog
// ---------------------------------------------------------------------------
export class StepDataDialog
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // StepDataDialog(QWidget *parent = Q_NULLPTR);
    constructor(parent)
    {
        // 原 `QDialog.call(this, parent)`(: QDialog(parent)) 已去掉 —— 本类不再"继承 Qt 对话框",
        // 而是**持一个真窗口**(this.ui, 见下), 窗口方法都转发到它。

        this.m_treeHeaderList1 = [];   // QStringList m_treeHeaderList1, m_treeHeaderList2;
        this.m_treeHeaderList2 = [];
        this.m_plot2Map = [];          // QVector<int> m_plot2Map;
        this.m_baseTime = 0;           // qint64 m_baseTime;  工步起始时间

        /* 最后一个工步信息 */
        this.m_stepBeginVolt = 0; this.m_stepEndVolt = 0;
        this.m_stepSetVolt = 0;      //工步设定电压
        this.m_stepBeginTime = 0; this.m_stepEndTime = 0;
        this.m_stepVolt = [];        // QVector<double> m_stepVolt;
        this.m_stepCap = [];         // QVector<double> m_stepCap;
        this.m_stepChargeType = 0;

        this.m_deviceId = "";        // QString m_deviceId;
        this.m_isReal = false;       // bool m_isReal;
        this.m_recordInfo = null;    // BTERecordPtr m_recordInfo;
        this.m_index = 0;            // int m_index;
        this.m_indexMax = 0;

        // 信号
        this.sigChannelChanged = [];   // signals: void sigChannelChanged(int idx);

        // 真 UI: 原版 StepDataDialog.ui 转换出的 XML; root 是**新窗口**(JZDialog), 不是 this。
        this.ui = loadUiFile('StepDataDialog.ui');
        // parent 传进来的是"持真窗口的 JS 对象"(如 BTEServer), 真控件在它的 .ui 上; 取对了再挂。
        if (parent) { const pw = parent.ui ? parent.ui : parent; if (pw) this.ui.setParentWidget(pw); }
        this.m_index = 0;
        this.m_isReal = false;

        // 原: flags |= Qt.WindowMinMaxButtonsHint | Qt.WindowCloseButtonHint; setWindowFlags(flags);
        // 引擎的窗口没有 Qt 的 windowFlags 概念(最小化/最大化/关闭按钮由框架统一给) —— 这几行去掉。

        this.m_treeHeaderList1.push(tr("记录序号"), tr("工步时间"), tr("起始电压(V)"), tr("终止电压(V)"), tr("充电中值电压(V)"),
            tr("放电中值电压(V)"), tr("恒流比(%)"), "");
        this.m_treeHeaderList2.push(tr("记录序号"), tr("记录时间"), tr("电压(V)"), tr("电流(A)"), tr("容量(Ah)"),
            tr("能量(Wh)"), tr("绝对时间"), tr("温度(℃)"));

        console.assert(this.m_treeHeaderList1.length === this.m_treeHeaderList2.length);   // Q_ASSERT(...)
        this.ui.treeWidget.setColumnCount(this.m_treeHeaderList1.length);

        // 原: setSelectionMode(QAbstractItemView.SingleSelection) / setSelectionBehavior(...SelectRows)
        //     / setEditTriggers(QTableWidget.NoEditTriggers) —— 引擎的表没有这几个开关(也没有 Qt 的
        //     枚举常量), 差别只在"能否多选 / 双击编辑", 先去掉(见回归报告)。
        // this.ui.treeWidget.setSelectionMode(QAbstractItemView.SingleSelection); //单击选择一行
        // this.ui.treeWidget.setSelectionBehavior(QAbstractItemView.SelectRows);  //设置只能选择一行，不能多行选
        // this.ui.treeWidget.setEditTriggers(QTableWidget.NoEditTriggers);

        // 原: this.ui.tabWidget.removeTab(2) —— 引擎的 TabWidget 没有 removeTab(页在 XML 里就全造出来了);
        //     原版是构造后删掉第 3 页, 这里先留着(多一个页), 待转换器支持"按 id 去掉某页"再处理。
        // this.ui.tabWidget.removeTab(2);

        this.updateTreeHeader(0);
    }

    // ~StepDataDialog();
    dispose()
    {
    }

    // void showEvent(QShowEvent *event);
    onShow(event)
    {
    }

    // QString deviceId();
    deviceId()
    {
        return this.m_deviceId;
    }

    // bool isReal();
    isReal()
    {
        return this.m_isReal;
    }

    // void updateTreeHeader(int level);
    updateTreeHeader(level)
    {
        // 原版的 QTreeWidget 按转换报告换成了 TableWidget; 引擎的表**有**表头文本接口
        // (JZTableWidget.setHeaderLabels -> C++ setHorizontalHeaderLabels), 这里照原样写回。
        if (level < 2)
            this.ui.treeWidget.setHeaderLabels(this.m_treeHeaderList1);
        else
            this.ui.treeWidget.setHeaderLabels(this.m_treeHeaderList2);
    }

    // void init(bool isReal, QString deviceString, int index, BTERecordPtr recordInfo);
    init(real, deviceString, index, recordInfo)
    {
        this.m_isReal = real;
        this.m_deviceId = deviceString;
        this.m_index = index;
        this.m_recordInfo = recordInfo;
        this.ui.treeWidget.clear();
        this.updateTreeHeader(0);

        this.m_stepBeginVolt = this.m_stepEndVolt = 0;
        this.m_stepBeginTime = this.m_stepEndTime = 0;
        this.m_stepSetVolt = -1;
        this.m_stepChargeType = STEP_CHARGE_OTHER;
        this.m_stepVolt = [];   // m_stepVolt.clear();
        this.m_stepCap = [];

        // setWindowTitle -> this.ui.setTitle(真窗口): 本类不再继承 QDialog, 标题设在 it.ui 上
        if (this.isReal())
            this.ui.setTitle(tr("实时数据 ") + deviceString + sprintf(" CH:%d-%d", (this.m_index / 8 | 0) + 1, this.m_index % 8 + 1));
        else
        {
            this.ui.setTitle(tr("离线数据 ") + deviceString + sprintf(" CH:%d-%d", (recordInfo.info.itemIndex / 8 | 0) + 1, recordInfo.info.itemIndex % 8 + 1));
        }

        if (this.m_recordInfo.dataList.length > 0)
            this.m_baseTime = aTimeToMSec(this.m_recordInfo.dataList[0].ATime);
        else
            this.m_baseTime = 0;

        this.ui.btnUpdate.setVisible(this.m_isReal);

        this.ui.label1L.setStyleSheet("color:blue;");
        this.ui.label2L.setStyleSheet("color:blue;");
        this.ui.label3L.setStyleSheet("color:blue;");
        this.ui.label1R.setStyleSheet("color:red;");
        this.ui.label2R.setStyleSheet("color:red;");
        this.ui.label3R.setStyleSheet("color:red;");

        this.ui.label1L.setText(tr("电流(A)"));
        this.ui.label1R.setText(tr("电压(V)"));

        this.ui.label2L.setText(tr("电压(V)"));

        this.ui.label3L.setText(tr("容量"));
        this.ui.label3R.setText(tr("比容量"));

        //plot time
        this.initPlot(this.ui.plot1, [tr("电流"), tr("电压")]);
        this.initPlot(this.ui.plot3, [tr("容量"), tr("比容量")]);

        this.ui.plot1.xAxis.setLabel(tr("时间(s)"));
        this.ui.plot2.xAxis.setLabel(tr("容量(Ah)"));

        this.ui.plot1.legend.setVisible(true);

        //plot2
        const stepList = this.m_recordInfo.info.stepInfo.step.stepList;
        let plot_num = 0;

        this.m_plot2Map = new Array(stepList.length).fill(-1);   // m_plot2Map.resize(...); m_plot2Map.fill(-1);

        const startStep = Math.max(this.m_recordInfo.info.stepInfo.startStepIndex - 1, 0);   // qMax(..., 0)
        for (let i = startStep; i < stepList.length; i++)
        {
            if (PackDefine.StepChargeType(stepList[i].StepType) !== STEP_CHARGE_OTHER)
            {
                this.m_plot2Map[i] = plot_num;
                plot_num++;
            }
        }

        this.ui.plot2.clearGraphs();
        for (let i = 0; i < plot_num; i++)
        {
            this.ui.plot2.addGraph();
        }
        this.ui.plot2.setInteractions(QCP.iRangeDrag | QCP.iRangeZoom);

        const key_time = [], val1_1 = [], val1_2 = [], val3_1 = [], val3_2 = [];
        const key2 = [];
        const val2 = [];
        for (let i = 0; i < plot_num; i++)
        {
            key2.push([]);   // key2.resize(plot_num);
            val2.push([]);   // val2.resize(plot_num);
        }

        //tree
        const step_count = this.m_recordInfo.dataList.length;
        for (let i = 0; i < step_count; i++)
        {
            const step = this.m_recordInfo.dataList[i];
            if (step.StepType === STEP_TYPE_STOP || step.StepType === STEP_TYPE_NULL)
                continue;

            const time = ((aTimeToMSec(step.ATime) - this.m_baseTime) / 1000) | 0;
            const current = DataTrans.fromElectricUnit(step.Current);
            const volt = DataTrans.fromVoltageUnit(step.Voltage);
            const cap = DataTrans.fromCapUnit(step.ChargeCap);
            const dis_cap = DataTrans.fromCapUnit(step.DisChargeCap);

            const step_idx = step.StepID - 1;
            const plot2_idx = this.m_plot2Map[step_idx];

            //plot
            key_time.push(time);
            val1_1.push(current);
            val1_2.push(volt);
            val3_1.push(cap);
            val3_2.push(volt);

            if (plot2_idx >= 0)
            {
                if (PackDefine.StepChargeType(step.StepType) === STEP_CHARGE)
                    key2[plot2_idx].push(cap);
                else
                    key2[plot2_idx].push(dis_cap);

                val2[plot2_idx].push(volt);
            }

            if (i < step_count - 1 && step.StepID === this.m_recordInfo.dataList[i + 1].StepID)
                this.addTreeStep(step, 1);
            else
                this.addTreeStep(step, 2);
        }
        this.updateLastStepItem();

        //绘制曲线
        this.ui.plot1.graph(0).setData(key_time, val1_1);
        this.ui.plot1.graph(1).setData(key_time, val1_2);
        this.ui.plot3.graph(0).setData(key_time, val3_1);
        this.ui.plot3.graph(1).setData(key_time, val3_2);

        for (let i = 0; i < this.ui.plot2.graphCount(); i++)
            this.ui.plot2.graph(i).setData(key2[i], val2[i]);

        this.on_btnPlotReset_clicked();
    }

    // void initPlot(QCustomPlot *customPlot, QStringList names);
    initPlot(customPlot, names)
    {
        // QSharedPointer<QCPAxisTickerTime> timeTicker(new QCPAxisTickerTime);
        const timeTicker = new QCPAxisTickerTime();
        timeTicker.setTimeFormat("%h:%m:%s");

        customPlot.clearGraphs();
        customPlot.xAxis.setTicker(timeTicker);

        customPlot.addGraph(customPlot.xAxis, customPlot.yAxis);
        customPlot.graph(0).setPen(new JZPen("blue")); // line color blue for first graph

        customPlot.addGraph(customPlot.xAxis, customPlot.yAxis2);
        customPlot.graph(1).setPen(new JZPen("red")); // line color red for second graph

        // pass data points to graphs:
        customPlot.graph(0).setName(names[0]);

        if (names.length > 1)
        {
            customPlot.graph(1).setName(names[1]);
            customPlot.yAxis2.setVisible(true);
        }

        customPlot.yAxis.setBasePen(new JZPen(Qt.blue, 1));
        customPlot.yAxis2.setBasePen(new JZPen(Qt.red, 1));

        customPlot.setInteractions(QCP.iRangeDrag | QCP.iRangeZoom);
        customPlot.axisRect().setRangeZoomAxes([customPlot.xAxis], [customPlot.yAxis, customPlot.yAxis2]);
        customPlot.axisRect().setRangeDragAxes([customPlot.xAxis], [customPlot.yAxis, customPlot.yAxis2]);

        customPlot.replot(QCustomPlot.rpQueuedReplot);
    }

    // void rescaleAxes();   //重置图像坐标
    rescaleAxes()
    {
        this.ui.plot1.graph(0).rescaleAxes(true);
        this.ui.plot1.graph(1).rescaleAxes(true);
        this.ui.plot1.replot(QCustomPlot.rpQueuedReplot);

        for (let i = 0; i < this.ui.plot2.graphCount(); i++)
        {
            this.ui.plot2.graph(i).rescaleAxes(true);
        }
        this.ui.plot2.replot(QCustomPlot.rpQueuedReplot);

        this.ui.plot3.graph(0).rescaleAxes(true);
        this.ui.plot3.graph(1).rescaleAxes(true);
        this.ui.plot3.replot(QCustomPlot.rpQueuedReplot);
    }

    // void addRealStep(QString deviceId, int index, const stru_BTS_CMD_SEND_DFDATA76::Data &data);
    addRealStep(deviceId, index, data)
    {
        //if (m_isReal && m_deviceId == deviceId && index == m_index)
        //	addStep(data);
    }

    // void addStep(const stru_BTS_CMD_SEND_DFDATA76::Data &data);
    addStep(step)
    {
        if (step.StepType === STEP_TYPE_STOP || step.StepType === STEP_TYPE_NULL)
            return;

        const time = ((aTimeToMSec(step.ATime) - this.m_baseTime) / 1000) | 0;
        const current = DataTrans.fromElectricUnit(step.Current);
        const volt = DataTrans.fromVoltageUnit(step.Voltage);
        const cap = DataTrans.fromCapUnit(step.ChargeCap);
        const dis_cap = DataTrans.fromCapUnit(step.DisChargeCap);

        const step_idx = step.StepID - 1;

        //plot
        this.ui.plot1.graph(0).addData(time, current);
        this.ui.plot1.graph(1).addData(time, volt);

        if (this.m_plot2Map.length > 0)
        {
            const plot2_idx = this.m_plot2Map[step_idx];
            if (plot2_idx >= 0)
            {
                const line = this.ui.plot2.graph(plot2_idx);
                if (PackDefine.StepChargeType(step.StepType) === STEP_CHARGE)
                    line.addData(cap, volt);
                else
                    line.addData(dis_cap, volt);
            }
        }

        this.ui.plot3.graph(0).addData(time, cap);
        this.ui.plot3.graph(1).addData(time, volt);

        this.rescaleAxes();

        this.addTreeStep(step, 0);
    }


    // void addTreeStep(const stru_BTS_CMD_SEND_DFDATA76::Data &step, int checkEnd);
    addTreeStep(step, addType)
    {
        //tree
        const prec = 4;
        const top_num = this.ui.treeWidget.topLevelItemCount();
        const loop_time = step.CycleStatus;

        let cap = 0;
        let eng = 0;
        const volt = DataTrans.fromVoltageUnit(step.Voltage);
        const time_ms = aTimeToMSec(step.ATime);
        const charge_type = PackDefine.StepChargeType(step.StepType);

        if (charge_type === STEP_CHARGE)
        {
            cap = DataTrans.fromCapUnit(step.ChargeCap);
            eng = DataTrans.fromEngUnit(step.ChargeEng);
        }
        else if (charge_type === STEP_DISCHARGE)
        {
            cap = DataTrans.fromCapUnit(step.DisChargeCap);
            eng = DataTrans.fromEngUnit(step.DisChargeEng);
        }

        //循环项
        let loop_item = null;
        if (loop_time >= top_num)
        {
            loop_item = new QTreeWidgetItem();
            loop_item.setText(0, tr("循环-") + String(top_num + 1));

            this.ui.treeWidget.addTopLevelItem(loop_item);
        }
        else
            loop_item = this.ui.treeWidget.topLevelItem(loop_time);

        const step_num = loop_item.childCount();
        let step_item = (step_num === 0) ? null : loop_item.child(step_num - 1);
        if (!step_item || Number(step_item.data(0, 0)) !== step.StepID)
        {
            this.updateLastStepItem();

            //添加新的子项
            step_item = new QTreeWidgetItem();
            step_item.setText(0, String(step_num + 1) + "-" + PackDefine.StepNameList[step.StepType]);
            step_item.setData(0, 0, step.StepID);
            loop_item.addChild(step_item);

            this.m_stepChargeType = charge_type;
            this.m_stepBeginVolt = volt;
            this.m_stepBeginTime = time_ms;
            this.m_stepSetVolt = this.stepSetVoltage(step.StepID);
            this.m_stepVolt = [];   // m_stepVolt.clear();
            this.m_stepCap = [];
        }

        this.m_stepVolt.push(volt);
        this.m_stepCap.push(cap);
        this.m_stepEndVolt = volt;
        this.m_stepEndTime = time_ms;

        //item_num
        let item_num = step_item.childCount();
        if (step_item.childCount() > 0)
            item_num += step_item.child(0).childCount();

        const sub_item = new QTreeWidgetItem();
        sub_item.setText(0, String(item_num + 1));
        sub_item.setText(1, DataTrans.fromTimeUnit(step.TestTime));
        sub_item.setText(2, volt.toFixed(prec));
        sub_item.setText(3, DataTrans.fromElectricUnit(step.Current).toFixed(prec));
        sub_item.setText(4, cap.toFixed(prec));
        sub_item.setText(5, eng.toFixed(prec));
        sub_item.setText(6, aTimeToISO(step.ATime));
        sub_item.setText(7, DataTrans.fromTempUnit(step.Tempature).toFixed(1));

        /*需求要求第一项和最后一项独立显示，其他项作为在第一项的子项，但是整体添加时每次判断较慢，这里做判断*/
        if (addType === 0)  //普通情况
        {
            if (step_item.childCount() === 2)
            {
                const last = step_item.child(1);
                step_item.removeChild(last);
                step_item.child(0).addChild(last);
            }
            step_item.addChild(sub_item);
        }
        else
        {
            //整体添加时
            if (step_item.childCount() === 0)
                step_item.addChild(sub_item);
            else
            {
                if (addType === 1)       //第一项
                    step_item.child(0).addChild(sub_item);
                else if (addType === 2)  //第二项
                    step_item.addChild(sub_item);
            }
        }
    }

    // void on_btnPlotReset_clicked();
    on_btnPlotReset_clicked()
    {
        this.ui.plot1.xAxis.setRange(0, 5);
        this.ui.plot1.yAxis.setRange(0, 5);
        this.ui.plot1.yAxis2.setRange(0, 5);

        this.ui.plot2.xAxis.setRange(0, 0.5);
        this.ui.plot2.yAxis.setRange(0, 5);
        this.ui.plot2.yAxis2.setRange(0, 5);

        this.ui.plot3.xAxis.setRange(0, 5);
        this.ui.plot3.yAxis.setRange(0, 5);
        this.ui.plot3.yAxis2.setRange(0, 5);

        this.rescaleAxes();
    }

    // void on_btnUpdate_clicked();
    on_btnUpdate_clicked()
    {
        if (this.m_index >= 0)
            emit(this.sigChannelChanged, this.m_index);   // emit sigChannelChanged(m_index);
    }

    // void on_btnUpChannel_clicked();
    on_btnUpChannel_clicked()
    {
        if (this.m_index > 0)
            emit(this.sigChannelChanged, this.m_index - 1);
    }

    // void on_btnDownChannel_clicked();
    on_btnDownChannel_clicked()
    {
        emit(this.sigChannelChanged, this.m_index + 1);
    }

    // void on_treeWidget_itemExpanded(QTreeWidgetItem *item);
    on_treeWidget_itemExpanded(item)
    {
    }

    // void on_treeWidget_currentItemChanged(QTreeWidgetItem *current, QTreeWidgetItem *previous);
    on_treeWidget_currentItemChanged(current, previous)
    {
        let level = 0;
        let item = current;
        while (item && item.parent())
        {
            item = item.parent();
            level++;
        }

        this.updateTreeHeader(level);
    }

    // void on_tabWidget_currentChanged(int index);
    on_tabWidget_currentChanged(index)
    {

    }


    // void updateLastStepItem();   //计算最后一个工步的充放电中值电压,恒流比
    updateLastStepItem()
    {
        const prec = 4;
        const count = this.ui.treeWidget.topLevelItemCount();

        let step_item = null;
        for (let i = count - 1; i >= 0; i--)
        {
            const top = this.ui.treeWidget.topLevelItem(i);
            if (top.childCount() > 0)
            {
                step_item = top.child(top.childCount() - 1);
                break;
            }
        }

        if (step_item)
        {
            //计算充放电中值电压
            let volt = 0;
            if (this.m_stepChargeType !== STEP_CHARGE_OTHER && this.m_stepCap.length > 0)
            {
                const var_ = this.m_stepCap[this.m_stepCap.length - 1] / 2;   // m_stepCap.back() / 2
                // std::equal_range(升序) 的 first -> 第一个 >= var 的位置
                let idx = this.m_stepCap.findIndex((v) => v >= var_);
                if (idx < 0)
                    idx = this.m_stepCap.length;
                idx = Math.max(idx, 0);                        // qMax(idx, 0)
                idx = Math.min(idx, this.m_stepVolt.length - 1); // qMin(idx, m_stepVolt.size() - 1)

                volt = this.m_stepVolt[idx];
            }

            step_item.setText(1, DataTrans.fromTimeUnit(this.m_stepEndTime - this.m_stepBeginTime));
            step_item.setText(2, this.m_stepBeginVolt.toFixed(prec));
            step_item.setText(3, this.m_stepEndVolt.toFixed(prec));
            step_item.setText(4, Number(this.m_stepChargeType === STEP_CHARGE ? volt : 0).toFixed(prec));
            step_item.setText(5, Number(this.m_stepChargeType === STEP_DISCHARGE ? volt : 0).toFixed(prec));
            step_item.setText(6, this.ccRatioText());
        }
    }

    // double stepSetVoltage(int stepID);  //工步设定电压,无设定返回-1
    stepSetVoltage(stepID)
    {
        const stepList = this.m_recordInfo.info.stepInfo.step.stepList;
        const idx = stepID - 1;
        if (idx < 0 || idx >= stepList.length)
            return -1;

        const step = stepList[idx];
        if (!step.isVoltageVaild())
            return -1;

        return step.Voltage();
    }

    // QString ccRatioText();              //计算恒流比 Qcc/(Qcc+Qcv)*100%
    ccRatioText()
    {
        //恒流比只在充电工步中计算
        if (this.m_stepChargeType !== STEP_CHARGE || this.m_stepCap.length === 0)
            return "";

        //恒流充电没有恒压阶段,恒流比为100%
        if (this.m_stepSetVolt <= 0)
            return (100.0).toFixed(2);

        const base = this.m_stepCap[0];        // m_stepCap.front()
        const total = this.m_stepCap[this.m_stepCap.length - 1] - base;   // m_stepCap.back() - base
        if (total <= 0)
            return "";

        //Qcc:电压达到设定值时已充入的容量(恒流阶段容量)
        let qcc = total;
        for (let i = 0; i < this.m_stepVolt.length; i++)
        {
            if (this.m_stepVolt[i] >= this.m_stepSetVolt)
            {
                qcc = this.m_stepCap[i] - base;
                break;
            }
        }

        return (qcc / total * 100.0).toFixed(2);
    }
}
