// ---------------------------------------------------------------------------
// BTEItemWidget.js —— 对应 C++ BTEServer/BTEItemWidget.h + BTEItemWidget.cpp
//
// 单个通道卡片: 自绘背景色/边框/标题/9 项数值, 支持勾选(左键切换)、右键菜单。
//
// Qt -> JS: s << x -> push; QColor 比较 -> a.equals(b); 出参 QColor&/QStringList&
//           -> 返回对象/数组; 信号 -> 回调数组字段 + emit(slots, ...)。
// ---------------------------------------------------------------------------

import { emit, tr, DeviceInfo } from './BTEServer.js';
import { PackDefine, STEP_TYPE_NULL, STEP_TYPE_1, STEP_TYPE_2, STEP_TYPE_3, STEP_TYPE_7, STEP_TYPE_STOP, stru_BTS_CMD_SEND_RTDATA76 } from './structs.js';
import { DataTrans } from './DataTrans.js';

// enum { Item_Normal, Item_Small, };
export const Item_Normal = 0;
export const Item_Small = 1;

//故障卡片配色:未清除为红色,已清除报警确认后为橙色
const FaultBgColor = new QColor(180, 40, 80);
const FaultClearedBgColor = new QColor(255, 140, 0);

// ---------------------------------------------------------------------------
// class BTEItemWidget : public QWidget
// ---------------------------------------------------------------------------
export class BTEItemWidget
{
    // BTEItemWidget(QWidget *parent = Q_NULLPTR);
    constructor(parent)
    {
        QWidget.call(this, parent);   // : QWidget(parent)

        this.m_mouseDown = false;
        this.m_checked = false;
        this.m_status = DeviceInfo.Status_Complete;
        this.m_alarmCleared = false;

        this.m_rtdata = new stru_BTS_CMD_SEND_RTDATA76();   // memset(&m_rtdata, 0, sizeof(m_rtdata));
        this.setContextMenuPolicy(Qt.CustomContextMenu);

        this.m_labels = [];
        this.m_labels.push(tr("状态"), tr("工步序号"), tr("循环序号"), tr("工步时间"), tr("电压(V)"),
            tr("电流(A)"), tr("温度(℃)"), tr("充电容量(Ah)"), tr("充电能量(Wh)"));

        this.m_scale = 1;
        this.m_fontTitle = this.font();
        this.m_fontTitle.setPixelSize(14);

        this.setSize(Item_Normal);
        this.updateToolTip();

        // 信号(基类有 customContextMenuRequested; 这里显式初始化, 保证 push 安全)
        this.sigCustomContextMenuRequested = [];
        this.sigToggled = [];   // signals: void toggled(bool checked);
    }

    // ~BTEItemWidget();
    dispose()
    {
    }

    // void getDispInfo(QStringList &vars);
    //   出参 QStringList& -> 返回数组
    getDispInfo()
    {
        const vars = [];
        let status = tr("正常");
        if (this.m_rtdata.workStatus() === stru_BTS_CMD_SEND_RTDATA76.WORK_ERROR_STOP || this.m_rtdata.PrtCode !== 0)
            status = PackDefine.ErrorText(this.m_rtdata.PrtCode);

        for (let i = 0; i < this.m_labels.length; i++)
            vars.push("");

        vars[0] = status;
        vars[1] = String(this.m_rtdata.StepID);
        vars[2] = String(this.m_rtdata.CycleStatus);
        vars[3] = DataTrans.fromTimeUnitSec(this.m_rtdata.TestTime);
        vars[4] = DataTrans.fromElectricUnit(this.m_rtdata.Voltage).toFixed(4);
        vars[5] = DataTrans.fromVoltageUnit(this.m_rtdata.Current).toFixed(4);
        vars[6] = DataTrans.fromTempUnit(this.m_rtdata.Tempature).toFixed(1);
        if (this.m_rtdata.StepType === STEP_TYPE_2)
        {
            this.m_labels[7] = tr("放电容量(Ah)");
            this.m_labels[8] = tr("放电能量(Wh)");
            vars[7] = DataTrans.fromCapUnit(this.m_rtdata.DisChargeCap).toFixed(4);
            vars[8] = DataTrans.fromEngUnit(this.m_rtdata.DisChargeEng).toFixed(4);
        }
        else
        {
            this.m_labels[7] = tr("充电容量(Ah)");
            this.m_labels[8] = tr("充电能量(Wh)");
            vars[7] = DataTrans.fromCapUnit(this.m_rtdata.ChargeCap).toFixed(4);
            vars[8] = DataTrans.fromEngUnit(this.m_rtdata.ChargeEng).toFixed(4);
        }

        return vars;
    }

    // virtual void paintEvent(QPaintEvent *event) override;
    paintEvent(event)
    {
        const painter = new QPainter(this);
        const black = new QPen(Qt.black);

        let bg_color = new QColor(192, 192, 192);
        if (!this.isEnabled())
            bg_color = new QColor(170, 175, 186);

        let ch_color = new QColor(128, 64, 0);
        const title_color = new QColor(0, 0, 0);
        let status_color = new QColor(0, 0, 0);

        const rc = this.rect();
        const w = this.width();
        const h = this.height();

        painter.fillRect(rc, bg_color);
        painter.setPen(new QPen(new QColor(129, 129, 129), 2));

        const border_gap = 6;
        const border_top = border_gap + 1, border_topw = 80, border_toph = 16;
        const border_top2 = border_top + border_toph;
        const border_top_x1 = ((w - border_topw) / 2) | 0;
        const border_top_x2 = border_top_x1 + border_topw;
        const border_left = border_gap;
        const border_right = w - border_gap;
        const border_bottom = h - border_gap;

        const cellColor = this.bgColor();
        if (!cellColor.equals(bg_color))
        {
            painter.fillRect(new QRect(new QPoint(border_top_x1, border_top), new QPoint(border_top_x2, border_top2)), cellColor);
            painter.fillRect(new QRect(new QPoint(border_left, border_top2), new QPoint(border_right, border_bottom)), cellColor);
        }

        //上
        painter.drawLine(new QPoint(border_top_x1, border_top), new QPoint(border_top_x2, border_top));
        painter.drawLine(new QPoint(border_top_x1, border_top), new QPoint(border_top_x1, border_top2));
        painter.drawLine(new QPoint(border_left, border_top2), new QPoint(border_top_x1, border_top2));
        painter.drawLine(new QPoint(border_top_x2, border_top2), new QPoint(border_right, border_top2));

        //左
        painter.drawLine(new QPoint(border_left, border_top2), new QPoint(border_left, border_bottom));

        //右下
        painter.setPen(new QPen(Qt.white, 2));
        painter.drawLine(new QPoint(border_top_x2, border_top), new QPoint(border_top_x2, border_top2));
        painter.drawLine(new QPoint(border_right, border_top2), new QPoint(border_right, border_bottom));
        painter.drawLine(new QPoint(border_left, border_bottom), new QPoint(border_right, border_bottom));

        if (this.m_checked)
        {
            const border = new QRect(new QPoint(border_left, border_top), new QPoint(border_right, border_bottom));
            border.adjust(-2, -2, 1, 1);
            painter.setPen(new QPen(Qt.yellow, 2));
            painter.drawRect(border);
        }

        // getTitle(QString &title, QColor &color) -> JS 返回 { title, color }
        const titleRet = this.getTitle();
        const title = titleRet.title;
        const title_color_out = titleRet.color;

        let status = tr("正常");
        if (this.m_rtdata.workStatus() === stru_BTS_CMD_SEND_RTDATA76.WORK_ERROR_STOP || this.m_rtdata.PrtCode !== 0)
        {
            status_color = new QColor(200, 0, 0);
            status = PackDefine.ErrorText(this.m_rtdata.PrtCode);
        }
        if (cellColor.equals(FaultBgColor) || cellColor.equals(FaultClearedBgColor)) //反色
        {
            status_color = new QColor(0, 0, 0);
            ch_color = new QColor(0, 0, 0);
        }

        const vars = this.getDispInfo();

        const title_y = border_top2 + 24;
        const ch_rc = new QRect(new QPoint(border_top_x1, border_top), new QPoint(border_top_x2, border_top2));
        const title_rc = new QRect(new QPoint(border_left, border_top2), new QPoint(border_right - 20, title_y));

        painter.setPen(ch_color);
        painter.setFont(this.m_fontTitle);
        painter.drawText(ch_rc, this.m_channel, new QTextOption(Qt.AlignCenter));

        painter.setFont(this.m_font);
        painter.setPen(title_color);
        painter.drawText(title_rc, title, Qt.AlignBottom | Qt.AlignRight);

        painter.setPen(black);
        let text_x, text_y, text_gap, text_len;

        if (this.m_scale === 1)
        {
            text_x = border_left + 4;
            text_len = this.width() - text_x - 4;
        }
        else
        {
            text_x = (this.width() * 1 / 4) | 0;
            text_len = this.width() - 2 * text_x;
        }
        if (this.m_itemSize === Item_Normal)
            text_gap = ((text_len / 2) | 0) + (((text_len / 2) | 0) - 60);
        else
            text_gap = ((text_len / 2) | 0) + (((text_len / 2) | 0) - 45);
        text_y = title_y + 24;

        for (let i = 0; i < this.m_labels.length; i++)
        {
            painter.drawText(text_x, text_y, this.m_labels[i]);
            if (i === 0)
            {
                const metrics = new QFontMetrics(this.m_font);
                const text_width = metrics.width(vars[i]);

                painter.setPen(status_color);
                if (text_x + text_gap + text_width > this.width())
                {
                    painter.drawText(w - text_width - 8, text_y, vars[i]);
                }
                else
                    painter.drawText(text_x + text_gap, text_y, vars[i]);

                painter.setPen(black);
            }
            else
                painter.drawText(text_x + text_gap, text_y, vars[i]);

            if (this.m_itemSize === Item_Normal)
                text_y += 22;
            else
                text_y += 16;
        }
    }

    // virtual void mousePressEvent(QMouseEvent *event) override;
    mousePressEvent(event)
    {
        if (event.button() === Qt.LeftButton || event.button() === Qt.RightButton)
            this.m_mouseDown = true;

        QWidget.prototype.mousePressEvent.call(this, event);
    }

    // virtual void mouseReleaseEvent(QMouseEvent *event) override;
    mouseReleaseEvent(event)
    {
        const pos = event.pos();
        if (this.m_mouseDown && this.rect().contains(pos))
        {
            const old_check = this.m_checked;
            if (QApplication.keyboardModifiers() !== Qt.ControlModifier)
            {
                if (!this.m_checked)
                {
                    this.m_checked = true;
                    this.update();
                }

                if (event.button() === Qt.LeftButton
                    || (event.button() === Qt.RightButton && !old_check))
                    emit(this.sigToggled, true);   // emit toggled(true);
            }
            else
            {
                this.m_checked = !this.m_checked;
                this.update();
            }
        }

        this.m_mouseDown = false;
        QWidget.prototype.mouseReleaseEvent.call(this, event);
    }

    // void setCheck(bool flag);
    setCheck(flag)
    {
        if (this.m_checked !== flag)
        {
            this.m_checked = flag;
            this.update();
        }
    }

    // bool isChecked();
    isChecked()
    {
        return this.m_checked;
    }

    // void setSize(int itemSize);
    setSize(itemSize)
    {
        this.m_itemSize = itemSize;

        if (itemSize === Item_Normal)
        {
            this.m_font = this.font();
            this.m_font.setPixelSize(12);

            this.m_baseWidth = 170;
            this.m_baseHeight = 272; //增加温度显示
        }
        else if (itemSize === Item_Small)
        {
            this.m_font = this.font();
            this.m_font.setPixelSize(9);

            this.m_baseWidth = 120;
            this.m_baseHeight = 218; //增加温度显示
        }
        this.setScale(this.m_scale);
    }

    // QColor bgColor();
    bgColor()
    {
        if (!this.isEnabled())
            return new QColor(170, 175, 186);

        if (this.m_status === DeviceInfo.Status_Complete)
        {
            return new QColor(192, 192, 192);
        }
        else if (this.m_status === DeviceInfo.Status_Stop)
        {
            if (this.m_rtdata.PrtCode !== 0)
                return this.m_alarmCleared ? FaultClearedBgColor : FaultBgColor;
            else
                return new QColor(240, 240, 240);
        }
        else if (this.m_status === DeviceInfo.Status_OffLine && this.m_rtdata.PrtCode !== 0)
        {
            return this.m_alarmCleared ? FaultClearedBgColor : FaultBgColor;
        }
        else
        {
            if (this.m_rtdata.StepType === STEP_TYPE_1
                || this.m_rtdata.StepType === STEP_TYPE_3
                || this.m_rtdata.StepType === STEP_TYPE_7)
                return new QColor(15, 175, 0);
            else if (this.m_rtdata.StepType === STEP_TYPE_2)
                return new QColor(180, 180, 20);
            else if (this.m_rtdata.StepType === STEP_TYPE_STOP)
                return new QColor(192, 192, 192);
            else
                return new QColor(204, 255, 255);
        }
    }

    // void setItemIndex(int idx);
    setItemIndex(idx)
    {
        this.m_itemIndex = idx;
    }

    // int itemIndex();
    itemIndex()
    {
        return this.m_itemIndex;
    }

    // void setChannelText(QString ch);
    setChannelText(ch)
    {
        this.m_channel = ch;
    }

    // int status();
    status()
    {
        return this.m_status;
    }

    // void setStatus(int status);
    setStatus(status)
    {
        this.m_status = status;
        this.update();
    }

    // void setAlarmCleared(bool flag);
    setAlarmCleared(flag)
    {
        if (this.m_alarmCleared !== flag)
        {
            this.m_alarmCleared = flag;
            this.update();
        }
    }

    // bool isAlarmCleared();
    isAlarmCleared()
    {
        return this.m_alarmCleared;
    }

    // void setScale(int scale);
    setScale(scale)
    {
        const gap = 3;

        this.m_scale = scale;
        this.setFixedSize(this.m_baseWidth * scale + (scale - 1) * gap, this.m_baseHeight);
    }

    // int scale();
    scale()
    {
        return this.m_scale;
    }

    // void clear();
    clear()
    {
        this.m_rtdata = new stru_BTS_CMD_SEND_RTDATA76();   // memset(&m_rtdata, 0, sizeof(m_rtdata));
        this.m_alarmCleared = false;
        this.update();
    }

    // stru_BTS_CMD_SEND_RTDATA76 data();
    data()
    {
        return this.m_rtdata;
    }

    // void setData(const stru_BTS_CMD_SEND_RTDATA76 &pack);
    setData(pack)
    {
        this.m_rtdata = pack;
        this.updateToolTip();
        this.update();
    }

    // void updateToolTip();
    updateToolTip()
    {
        const vars = this.getDispInfo();

        let tips = "";
        for (let i = 0; i < this.m_labels.length; i++)
        {
            tips += this.m_labels[i] + ": " + vars[i] + "\n";
        }
        this.setToolTip(tips);
    }

    // void getTitle(QString &title, QColor &color);
    //   出参 -> 返回 { title, color }
    getTitle()
    {
        let title = "";
        let color = new QColor(0, 0, 0);

        const stepType = this.m_rtdata.StepType;
        if (this.m_status === DeviceInfo.Status_Complete)
            title = tr("完成");
        else if (this.m_status === DeviceInfo.Status_WaitStart)
            title = tr("正在启动");
        else if (this.m_status === DeviceInfo.Status_Run)
        {
            if (stepType === STEP_TYPE_NULL)
                title = tr("正在启动");
            else
                title = PackDefine.StepNameList[stepType];
        }
        else if (this.m_status === DeviceInfo.Status_WaitStop)
        {
            title = tr("正在停止");
        }
        else if (this.m_status === DeviceInfo.Status_Stop)
        {
            title = tr("暂停");
        }
        else if (this.m_status === DeviceInfo.Status_WaitJump)
        {
            title = tr("正在跳转");
        }
        else if (this.m_status === DeviceInfo.Status_Error)
        {
            title = tr("内部错误");
        }
        else if (this.m_status === DeviceInfo.Status_OffLine)
        {
            title = tr("离线");
        }
        else
        {
            console.assert(0);   // Q_ASSERT(0);
        }

        return { title: title, color: color };
    }
}
