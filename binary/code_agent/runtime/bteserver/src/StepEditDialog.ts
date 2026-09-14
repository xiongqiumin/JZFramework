// ---------------------------------------------------------------------------
// StepEditDialog.js —— 对应 C++ BTEServer/StepEditDialog.h + StepEditDialog.cpp
//
// 单个工步编辑对话框: 类型下拉(恒流恒压/恒压/恒流/恒流放电/静置/循环) + 参数行,
// 按类型显示/隐藏参数控件, 校验后写回 stru_BTS_CMD_SET_STEP76_Ex::StepInfo。
//
// Qt -> JS: 出参 double&out -> 返回 { ok, value }; QMap<int,int>::key(v,def) ->
//           自写 findKey; 类外 TimeValidator 移到文件末尾。
// ---------------------------------------------------------------------------

import { tr, sprintf } from './BTEServer.js';
import { PackDefine, STEP_TYPE_1, STEP_TYPE_2, STEP_TYPE_3, STEP_TYPE_7, STEP_TYPE_IDLE, STEP_TYPE_LOOP, STEP_TYPE_STOP, PARAM_Current, PARAM_Voltage, PARAM_StopCurrent, PARAM_StopVoltage, PARAM_Time, PARAM_Capacity, PARAM_Tempature, PARAM_CountLoop, PARAM_BeginStepID, OP_EQU, OP_BIG_EQU, OP_SMALL_EQU } from './PackDefine.js';
import { DataTrans } from './DataTrans.js';
import { g_project } from './BTEProject.js';

//工步容量设定上限(Ah),精度百分之一
const MAX_STEP_CAPACITY = 10000.0;
//工步温度设定上限(℃),精度0.5℃
const MAX_STEP_TEMPATURE = 300.0;

// ---------------------------------------------------------------------------
// class StepEditDialog : public QDialog
// ---------------------------------------------------------------------------
export class StepEditDialog
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // StepEditDialog(QWidget *parent = Q_NULLPTR);
    constructor(parent)
    {
        QDialog.call(this, parent);   // : QDialog(parent)

        this.itemIndex = 0;        // int itemIndex;
        this.DeviceString = "";    // QString DeviceString;

        this.m_typeMap = new Map();   // QMap<int, int> m_typeMap;
        this.m_paramWidgets = [];     // QVector<QWidget*> m_paramWidgets;

        this.ui = new Ui_StepEditDialogClass();
        this.ui.setupUi(this);

        this.m_typeMap.set(0, STEP_TYPE_7);
        this.m_typeMap.set(1, STEP_TYPE_3);
        this.m_typeMap.set(2, STEP_TYPE_1);
        this.m_typeMap.set(3, STEP_TYPE_2);
        this.m_typeMap.set(4, STEP_TYPE_IDLE);
        this.m_typeMap.set(5, STEP_TYPE_LOOP);

        for (const var_ of this.m_typeMap.values())
            this.ui.boxType.addItem(PackDefine.StepNameList[var_]);

        this.m_paramWidgets.push(this.ui.boxSetTime, this.ui.lineTime,
            this.ui.label2, this.ui.lineVolt, this.ui.label3, this.ui.lineCurrent,
            this.ui.label4, this.ui.lineStopCurrent, this.ui.label5, this.ui.lineStopVoltage,
            this.ui.label6, this.ui.lineStartStep, this.ui.label7, this.ui.lineLoopTime,
            this.ui.label8, this.ui.lineCapacity, this.ui.label9, this.ui.spinTempature);

        this.ui.lineTime.setInputMask("99:99:99:999");
        this.ui.lineTime.setText("00:00:00:000");

        const v = new TimeValidator(this);
        this.ui.lineTime.setValidator(v);

        //容量:0.01 - 10000.00 Ah,精度百分之一
        const capValidator = new QDoubleValidator(0.01, MAX_STEP_CAPACITY, 2, this);
        capValidator.setNotation(QDoubleValidator.StandardNotation);
        this.ui.lineCapacity.setValidator(capValidator);

        //温度:0 - 300 ℃,以0.5℃为单位
        this.ui.spinTempature.setRange(0, MAX_STEP_TEMPATURE);
        this.ui.spinTempature.setSingleStep(0.5);
        this.ui.spinTempature.setDecimals(1);
        this.ui.spinTempature.setSpecialValueText(tr("未设置"));
    }

    // ~StepEditDialog();
    dispose()
    {
    }

    // void showEvent(QShowEvent *event);
    showEvent(event)
    {
        this.stepToUi();
        this.on_boxType_currentIndexChanged(this.ui.boxType.currentIndex());

        QDialog.prototype.showEvent.call(this, event);   // __super::showEvent(event);
    }

    // bool isVaildParam(int stepType, int param); //是否有效参数
    isVaildParam(stepType, param)
    {
        //电流
        if (stepType === STEP_TYPE_1
            || stepType === STEP_TYPE_2
            || stepType === STEP_TYPE_7)
        {
            if (param === PARAM_Current)
                return true;
        }
        //电压
        if (stepType === STEP_TYPE_3
            || stepType === STEP_TYPE_7)
        {
            if (param === PARAM_Voltage)
                return true;
        }
        //截止电流
        if (stepType === STEP_TYPE_3
            || stepType === STEP_TYPE_7)
        {
            if (param === PARAM_StopCurrent)
                return true;
        }
        //截止电压
        if (stepType === STEP_TYPE_2
            || stepType === STEP_TYPE_1)
        {
            if (param === PARAM_StopVoltage)
                return true;
        }
        //时间
        if (stepType === STEP_TYPE_1
            || stepType === STEP_TYPE_2
            || stepType === STEP_TYPE_3
            || stepType === STEP_TYPE_IDLE
            || stepType === STEP_TYPE_7)
        {
            if (param === PARAM_Time)
                return true;
        }
        //容量,温度
        if (stepType === STEP_TYPE_1
            || stepType === STEP_TYPE_2
            || stepType === STEP_TYPE_3
            || stepType === STEP_TYPE_7)
        {
            if (param === PARAM_Capacity
                || param === PARAM_Tempature)
                return true;
        }
        //起始工步，循环次数
        if (stepType === STEP_TYPE_LOOP)
        {
            if (param === PARAM_CountLoop
                || param === PARAM_BeginStepID)
                return true;
        }

        return false;
    }

    // bool checkData(QString param_name, QString var, double max, double &out);
    //   出参 double& out -> 返回 { ok, value }
    checkData(param_name, var_str, max)
    {
        const ok = (var_str.trim() !== "") && !isNaN(Number(var_str));
        const var_ = Number(var_str);
        if (var_str === "")
        {
            JZMessageBox.information(null, "", param_name + tr(" 不能为空"));
            return { ok: false, value: 0 };
        }

        if (!ok)
        {
            JZMessageBox.information(null, "", param_name + tr("不正确，请检查"));
            return { ok: false, value: 0 };
        }

        if (var_ <= 0 || var_ > max)
        {
            const range = sprintf("(0-%0.4f)", max);
            JZMessageBox.information(null, "", param_name + tr("参数超出范围") + range + tr("，请检查"));
            return { ok: false, value: 0 };
        }

        return { ok: true, value: var_ };
    }

    // void stepToUi();
    stepToUi()
    {
        const prec = 4;

        this.ui.lineNum.setText(String(this.step.StepLine));
        this.ui.boxType.setCurrentIndex(findKey(this.m_typeMap, this.step.StepType, -1));   // m_typeMap.key(step.StepType, -1)

        if (this.step.isTimeVaild())
        {
            this.ui.lineTime.setText(DataTrans.fromTimeUnit(this.step.Time()));
            this.ui.boxSetTime.setChecked(true);
        }

        if (this.step.isVoltageVaild())
            this.ui.lineVolt.setText(Number(this.step.Voltage()).toFixed(prec));
        if (this.step.isCurrentVaild())
            this.ui.lineCurrent.setText(Number(this.step.Current()).toFixed(prec));
        if (this.step.isStopCurrentVaild())
            this.ui.lineStopCurrent.setText(Number(this.step.StopCurrent()).toFixed(prec));
        if (this.step.isStopVoltageVaild())
            this.ui.lineStopVoltage.setText(Number(this.step.StopVoltage()).toFixed(prec));

        //容量(Ah),百分之一精度
        this.ui.lineCapacity.clear();
        if (this.step.isCapacityVaild())
            this.ui.lineCapacity.setText(Number(this.step.Capacity()).toFixed(2));

        //温度(℃),0.5℃精度
        this.ui.spinTempature.setValue(0);
        if (this.step.isTempatureVaild())
        {
            let temp = Math.round(this.step.Tempature() * 2.0) / 2.0;   // qRound(x*2)/2
            temp = Math.min(temp, MAX_STEP_TEMPATURE);                  // qMin
            this.ui.spinTempature.setValue(temp);
        }
    }

    // bool uiToStep();
    uiToStep()
    {
        const lineVolt = this.ui.lineVolt.text().trim();
        const lineCurrent = this.ui.lineCurrent.text().trim();
        const lineStopCurrent = this.ui.lineStopCurrent.text().trim();
        const lineStopVoltage = this.ui.lineStopVoltage.text().trim();

        const scale = g_project.deviceData(this.DeviceString).scale(this.itemIndex);
        const volt_max = g_project.deviceData(this.DeviceString).maxVoltage() * scale;
        const current_max = g_project.deviceData(this.DeviceString).maxCurrent() * scale;

        this.step.clearParams();
        const stepType = this.m_typeMap.get(this.ui.boxType.currentIndex());
        this.step.StepType = stepType;

        if (this.isVaildParam(stepType, PARAM_Time))
        {
            if (this.ui.boxSetTime.isChecked())
            {
                const ms = DataTrans.toTimeUnit(this.ui.lineTime.text());
                this.step.setTime(OP_BIG_EQU, ms);
            }
        }

        if (this.isVaildParam(stepType, PARAM_Voltage))
        {
            const r = this.checkData(tr("电压"), lineVolt, volt_max);
            if (!r.ok)
                return false;

            this.step.setVoltage(OP_SMALL_EQU, r.value);
        }
        if (this.isVaildParam(stepType, PARAM_Current))
        {
            const r = this.checkData(tr("电流"), lineCurrent, current_max);
            if (!r.ok)
                return false;

            this.step.setCurrent(OP_SMALL_EQU, r.value);
        }
        if (this.isVaildParam(stepType, PARAM_StopCurrent))
        {
            const r = this.checkData(tr("截止电流"), lineStopCurrent, current_max);
            if (!r.ok)
                return false;

            this.step.setStopCurrent(OP_BIG_EQU, r.value);
        }
        if (this.isVaildParam(stepType, PARAM_StopVoltage))
        {
            const r = this.checkData(tr("截止电压"), lineStopVoltage, volt_max);
            if (!r.ok)
                return false;

            this.step.setStopVoltage(OP_BIG_EQU, r.value);
        }
        if (this.isVaildParam(stepType, PARAM_BeginStepID))
        {
            this.step.setBeginStepID(OP_EQU, this.ui.lineStartStep.value());
            this.step.setCountLoop(OP_EQU, this.ui.lineLoopTime.value());
        }
        //容量,超过设定值跳入下一工步
        if (this.isVaildParam(stepType, PARAM_Capacity))
        {
            const text = this.ui.lineCapacity.text().trim();
            if (text !== "")
            {
                const r = this.checkData(tr("容量"), text, MAX_STEP_CAPACITY);
                if (!r.ok)
                    return false;

                this.step.setCapacity(OP_BIG_EQU, r.value);
            }
        }
        //温度,超过设定值跳入下一工步
        if (this.isVaildParam(stepType, PARAM_Tempature))
        {
            const tempature = this.ui.spinTempature.value();
            if (tempature > 0)
                this.step.setTempature(OP_BIG_EQU, tempature);
        }

        if (this.step.StepType === STEP_TYPE_7)
        {
            //截止电流的值必须小于电流值
            if (Number(lineStopCurrent) > Number(lineCurrent))
            {
                JZMessageBox.information(null, "", tr("截止电流必须小于电流值"));
                return false;
            }
            //另外截止电流不能小于0.1
            if (Number(lineStopCurrent) < 0.1)
            {
                JZMessageBox.information(null, "", tr("截止电流不能小于0.1"));
                return false;
            }
        }

        //额外处理
        if (this.step.StepType !== STEP_TYPE_STOP)
        {
            this.step.setReservedParam1(OP_EQU, 0x41);
        }

        return true;
    }

    // void on_btnOk_clicked();
    on_btnOk_clicked()
    {
        if (!this.uiToStep())
            return;

        this.accept();
    }

    // void on_btnCancel_clicked();
    on_btnCancel_clicked()
    {
        this.reject();
    }

    // void on_boxType_currentIndexChanged(int index);
    on_boxType_currentIndexChanged(index)
    {
        for (const w of this.m_paramWidgets)
            w.setVisible(false);

        const stepType = this.m_typeMap.get(this.ui.boxType.currentIndex());
        this.ui.boxSetTime.setEnabled(true);

        //电流
        if (stepType === STEP_TYPE_1
            || stepType === STEP_TYPE_2
            || stepType === STEP_TYPE_7)
        {
            this.ui.label3.setVisible(true);
            this.ui.lineCurrent.setVisible(true);
        }
        //电压
        if (stepType === STEP_TYPE_3
            || stepType === STEP_TYPE_7)
        {
            this.ui.label2.setVisible(true);
            this.ui.lineVolt.setVisible(true);
        }
        //截止电流
        if (stepType === STEP_TYPE_3
            || stepType === STEP_TYPE_7)
        {
            this.ui.label4.setVisible(true);
            this.ui.lineStopCurrent.setVisible(true);
        }
        //截至电压
        if (stepType === STEP_TYPE_1
            || stepType === STEP_TYPE_2)
        {
            this.ui.label5.setVisible(true);
            this.ui.lineStopVoltage.setVisible(true);
        }
        //时间
        if (stepType === STEP_TYPE_1
            || stepType === STEP_TYPE_2
            || stepType === STEP_TYPE_3
            || stepType === STEP_TYPE_IDLE
            || stepType === STEP_TYPE_7)
        {
            if (stepType === STEP_TYPE_IDLE)
            {
                this.ui.boxSetTime.setChecked(true);
                this.ui.boxSetTime.setEnabled(false);
            }
            else
                this.ui.boxSetTime.setEnabled(true);

            this.ui.boxSetTime.setVisible(true);
            this.ui.lineTime.setVisible(true);
        }
        //起始工步，循环次数
        if (stepType === STEP_TYPE_LOOP)
        {
            this.ui.label6.setVisible(true);
            this.ui.label7.setVisible(true);
            this.ui.lineStartStep.setVisible(true);
            this.ui.lineLoopTime.setVisible(true);

            this.ui.lineStartStep.setRange(1, this.step.StepLine - 1);
            this.ui.lineStartStep.setValue(this.step.BeginStepID());
            this.ui.lineLoopTime.setValue(this.step.CountLoop());
        }
        //容量,温度
        if (stepType === STEP_TYPE_1
            || stepType === STEP_TYPE_2
            || stepType === STEP_TYPE_3
            || stepType === STEP_TYPE_7)
        {
            this.ui.label8.setVisible(true);
            this.ui.label9.setVisible(true);
            this.ui.lineCapacity.setVisible(true);
            this.ui.spinTempature.setVisible(true);
        }
    }
}

// QMap::key(value, default) 的等价实现(取第一个匹配的键)
function findKey(map, value, def)
{
    for (const [k, v] of map)
    {
        if (v === value)
            return k;
    }
    return def;
}

// ---------------------------------------------------------------------------
// C++ 里定义在 StepEditDialog.cpp 的类外校验器
// ---------------------------------------------------------------------------
// class TimeValidator : public QValidator
export class TimeValidator
{
    // TimeValidator(QObject *parent);
    constructor(parent)
    {
        QValidator.call(this, parent);   // :QValidator(parent)
    }

    // virtual State validate(QString &input, int &pos) const;
    validate(input, pos)
    {
        const list = input.split(":");
        if (list.length === 4 && Number(list[1]) < 60 && Number(list[2]) < 60)
            return QValidator.State.Acceptable;

        return QValidator.State.Invalid;
    }
}
