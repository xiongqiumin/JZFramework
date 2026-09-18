// ---------------------------------------------------------------------------
// BTEItemWidget.js —— 对应 C++ BTEServer/BTEItemWidget.h + BTEItemWidget.cpp
//
// 单个通道卡片: 自绘背景色/边框/标题/9 项数值, 支持勾选(左键切换)、右键菜单。
//
// Qt -> JS: s << x -> push; QColor 比较 -> a.equals(b); 出参 QColor&/QStringList&
//           -> 返回对象/数组; 信号 -> 回调数组字段 + emit(slots, ...)。
// ---------------------------------------------------------------------------

import { emit, tr, DeviceInfo } from './BTEServer.js';
import { stru_BTS_CMD_SEND_RTDATA76 } from './structs.js';
import { PackDefine, STEP_TYPE_1, STEP_TYPE_2, STEP_TYPE_3, STEP_TYPE_7, STEP_TYPE_NULL, STEP_TYPE_STOP } from './PackDefine.js';
import { DataTrans } from './DataTrans.js';

// ---------------------------------------------------------------------------
// 去 Qt 兼容层(2026-09-14): 原来的 QColor/QPen/QPoint/QRect/QPainter/QFontMetrics/
//   makeRealWidget 全换成引擎的真类型 —— JZColor/JZPen/JZPoint/JZRect/JZPainter/JZFont。
// ---------------------------------------------------------------------------

// Qt 鼠标/修饰键常量(值与原 Qt 一致): 引擎的 JZMouseEvent.button() 就是这套数字
// (0=NoButton 1=Left 2=Right 3=Middle), modifiers() 是位组合(1=Shift 2=Ctrl 4=Alt)。
const Qt = {
    LeftButton: 1,
    RightButton: 2,
    ControlModifier: 2,
};

// enum { Item_Normal, Item_Small, };
export const Item_Normal = 0;
export const Item_Small = 1;

//故障卡片配色:未清除为红色,已清除报警确认后为橙色
const FaultBgColor = new JZColor(180, 40, 80);
const FaultClearedBgColor = new JZColor(255, 140, 0);

// JZColor -> CSS 颜色串(JZPainter 的 fillRect/drawText 之类的"便捷形式"只收字符串)
function css(c) { return "#" + [c.red(), c.green(), c.blue()].map((v) => (v & 0xff).toString(16).padStart(2, "0")).join(""); }

// 文本宽度估算(引擎还没把字体度量暴露给 JS —— 原 QFontMetrics::width 的近似替代)
// TODO(引擎): JZFont 加上 measureText()/width() 之后换成真度量。
function textWidth(font, text) { return Math.ceil(String(text).length * (font.size() * 0.62)); }

// 引擎的 JZPainter 只有 drawText(x, y, text)(Qt 的基线语义), 没有
// drawText(rect, text, flags) —— 这里按"矩形 + 对齐"自己算位置(替代 QTextOption/AlignXXX)。
function drawTextCenter(painter, x, y, w, h, text, font)
{
    const tw = textWidth(font, text);
    painter.drawText(x + (((w - tw) / 2) | 0), y + ((((h + font.size()) / 2) | 0) - 2), String(text));
}

function drawTextBottomRight(painter, x, y, w, h, text, font)
{
    const tw = textWidth(font, text);
    painter.drawText(x + w - tw, y + h - 2, String(text));
}

// ---------------------------------------------------------------------------
// "真控件化": `new BTEItemWidget()` 直接得到一个**真 JZWidget**
//
//   不能 `class BTEItemWidget extends JZWidget`: jsbind 的构造返回原生包装对象,
//   派生原型会被丢掉(类方法全丢)。所以约定写成:
//       constructor(parent) { return makeRealWidget(this, JZWidget, parent); }
//       initWidget(parent)  { ...原来的构造逻辑, this = 真控件... }
//   makeRealWidget 做四件事: 建真控件 -> 把类原型上的方法挂到实例 -> 接上引擎的
//   onPaint/onMouseXxx 钩子 -> 调 initWidget(如果有)。
//   (原实现来自已删除的 qtcompat.ts, 这里就地保留同一套语义。)
// ---------------------------------------------------------------------------
function makeRealWidget(self, Jz, ...args)
{
    const Cls = (self && self.constructor && self.constructor !== Object) ? self.constructor : null;
    const real = new Jz();

    if (Cls)
    {
        const names = Object.getOwnPropertyNames(Cls.prototype);
        for (const k of names)
        {
            if (k === "constructor")
                continue;
            const d = Object.getOwnPropertyDescriptor(Cls.prototype, k);
            if (!d)
                continue;
            if (d.get || d.set)
                Object.defineProperty(real, k, d);
            else
                real[k] = d.value;
        }
    }

    // 引擎的钩子 -> 本类的 Qt 风格方法(事件对象直接用 JZ 的 JZMouseEvent/JZPainterEvent)
    const hook = (jzName, fnName) =>
    {
        real[jzName] = (e) =>
        {
            const fn = real[fnName];
            if (typeof fn !== "function")
                return;
            try { fn.call(real, e); }
            catch (ex) { console.error("[BTEItemWidget] " + fnName + " 异常: " + ex); }
        };
    };
    hook("onPaint", "paintEvent");
    hook("onMousePress", "mousePressEvent");
    hook("onMouseRelease", "mouseReleaseEvent");
    hook("onMouseMove", "mouseMoveEvent");

    if (typeof real.initWidget === "function")
        real.initWidget.apply(real, args);
    return real;
}

// ---------------------------------------------------------------------------
// class BTEItemWidget : public QWidget
// ---------------------------------------------------------------------------
export class BTEItemWidget
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // BTEItemWidget(QWidget *parent = Q_NULLPTR);
    //
    // 真控件化(2026-09-13): 不再用 `QWidget.call(this, parent)` 那种"JS 对象冒充控件"的写法,
    // 而是返回一个**真 JZWidget**: makeRealWidget 建真控件 + 把本类原型上的方法挂到实例 +
    // 把 onPaint/onMouseXxx 钩子接回 paintEvent/mousePressEvent/...(事件按 Qt 形状包好)。
    // 构造逻辑挪到 initWidget(this = 真控件)。
    // parent 可省(原 C++ 的默认实参 Q_NULLPTR): 实际由布局 addWidget 建立父子关系。
    constructor(parent = null)
    {
        return makeRealWidget(this, JZWidget, parent);
    }

    // void initWidget(QWidget *parent);   —— 构造后的初始化(this = 真控件)
    initWidget(parent)
    {
        this.m_mouseDown = false;
        this.m_checked = false;
        this.m_status = DeviceInfo.Status_Complete;
        this.m_alarmCleared = false;

        this.m_rtdata = new stru_BTS_CMD_SEND_RTDATA76();   // memset(&m_rtdata, 0, sizeof(m_rtdata));
        // 原 setContextMenuPolicy(Qt.CustomContextMenu): 引擎的控件没有"右键策略"这个开关,
        // 右键固定走 onMousePress(button() == 2) —— 见 mousePressEvent 里的 emit。
        this.m_enabled = true;   // 引擎的 JZWidget 还没有 enabled 状态 -> 本地记账(见 setEnabled/isEnabled)

        this.m_labels = [];
        this.m_labels.push(tr("状态"), tr("工步序号"), tr("循环序号"), tr("工步时间"), tr("电压(V)"),
            tr("电流(A)"), tr("温度(℃)"), tr("充电容量(Ah)"), tr("充电能量(Wh)"));

        this.m_scale = 1;
        this.m_fontTitle = new JZFont(14);   // 原 this.font() + setPixelSize(14)

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
        // this 运行期就是真 JZWidget(见 makeRealWidget), 类型上仍是 JS 类实例 ⇒ 断言一次
        const painter = new JZPainter(this as any);
        const black = new JZPen("#000000");

        let bg_color = new JZColor(192, 192, 192);
        if (!this.isEnabled())
            bg_color = new JZColor(170, 175, 186);

        let ch_color = new JZColor(128, 64, 0);
        const title_color = new JZColor(0, 0, 0);
        let status_color = new JZColor(0, 0, 0);

        const rc = this.rect();
        const w = this.width();
        const h = this.height();

        // 原 painter.fillRect(rect(), bg_color): JZ 的 fillRect 是 (x, y, w, h, 颜色串)
        painter.fillRect(rc.x(), rc.y(), rc.width(), rc.height(), css(bg_color));
        painter.setPen(new JZPen("#818181", 2));

        const border_gap = 6;
        const border_top = border_gap + 1, border_topw = 80, border_toph = 16;
        const border_top2 = border_top + border_toph;
        const border_top_x1 = ((w - border_topw) / 2) | 0;
        const border_top_x2 = border_top_x1 + border_topw;
        const border_left = border_gap;
        const border_right = w - border_gap;
        const border_bottom = h - border_gap;

        const cellColor = this.bgColor();
        if (!cellColor.equal(bg_color))   // 原 QColor::operator!= -> JZColor.equal
        {
            // 原 new QRect(两点的 QPoint 构造) -> 宽高 = |Δ| + 1(QRect 是闭区间)
            painter.fillRect(border_top_x1, border_top,
                border_top_x2 - border_top_x1 + 1, border_top2 - border_top + 1, css(cellColor));
            painter.fillRect(border_left, border_top2,
                border_right - border_left + 1, border_bottom - border_top2 + 1, css(cellColor));
        }

        //上
        painter.drawLine(border_top_x1, border_top, border_top_x2, border_top);
        painter.drawLine(border_top_x1, border_top, border_top_x1, border_top2);
        painter.drawLine(border_left, border_top2, border_top_x1, border_top2);
        painter.drawLine(border_top_x2, border_top2, border_right, border_top2);

        //左
        painter.drawLine(border_left, border_top2, border_left, border_bottom);

        //右下
        painter.setPen(new JZPen("#ffffff", 2));
        painter.drawLine(border_top_x2, border_top, border_top_x2, border_top2);
        painter.drawLine(border_right, border_top2, border_right, border_bottom);
        painter.drawLine(border_left, border_bottom, border_right, border_bottom);
        if (this.m_checked)
        {
            // 原: new QRect(两点) 再 adjust(-2, -2, 1, 1)
            const x1 = border_left - 2, y1 = border_top - 2;
            const x2 = border_right + 1, y2 = border_bottom + 1;
            painter.setPen(new JZPen("#ffff00", 2));
            painter.drawRect(x1, y1, x2 - x1 + 1, y2 - y1 + 1);
        }
        // getTitle(QString &title, QColor &color) -> JS 返回 { title, color }
        const titleRet = this.getTitle();
        const title = titleRet.title;
        const title_color_out = titleRet.color;

        let status = tr("正常");
        if (this.m_rtdata.workStatus() === stru_BTS_CMD_SEND_RTDATA76.WORK_ERROR_STOP || this.m_rtdata.PrtCode !== 0)
        {
            status_color = new JZColor(200, 0, 0);
            status = PackDefine.ErrorText(this.m_rtdata.PrtCode);
        }
        if (cellColor.equal(FaultBgColor) || cellColor.equal(FaultClearedBgColor)) //反色
        {
            status_color = new JZColor(0, 0, 0);
            ch_color = new JZColor(0, 0, 0);
        }

        const vars = this.getDispInfo();

        const title_y = border_top2 + 24;
        // 原来用 QRect 定位文本(ch_rc / title_rc); JZPainter 没有"矩形 + 对齐"的 drawText,
        // 所以这里把矩形拆成 x/y/w/h 交给上面的两个小助手(对齐方式不变)。
        const ch_x = border_top_x1, ch_y = border_top;
        const ch_w = border_top_x2 - border_top_x1 + 1, ch_h = border_top2 - border_top + 1;
        const title_x = border_left, title_w = (border_right - 20) - border_left + 1;

        painter.setPen(ch_color);
        painter.setFont(this.m_fontTitle);
        drawTextCenter(painter, ch_x, ch_y, ch_w, ch_h, this.m_channel, this.m_fontTitle);

        painter.setFont(this.m_font);
        painter.setPen(title_color);
        drawTextBottomRight(painter, title_x, border_top2, title_w, title_y - border_top2 + 1, title, this.m_font);

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
                // 原 QFontMetrics(font).width(text) —— 引擎没暴露字体度量, 用 textWidth() 近似
                const text_width = textWidth(this.m_font, vars[i]);

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
    onMousePress(event)
    {
        if (event.button() === 1 || event.button() === 2)   // JZMouseEvent.Button: 1=左 2=右(原 Qt.LeftButton/RightButton)
            this.m_mouseDown = true;

        // 原 Qt 的 setContextMenuPolicy(Qt.CustomContextMenu): 引擎控件没有那个开关,
        // 右键固定在这里发"请求上下文菜单"(带的点就是控件局部坐标)。
        if (event.button() === 2)
            emit(this.sigCustomContextMenuRequested, new JZPoint(event.localX(), event.localY()));
    }

    // virtual void mouseReleaseEvent(QMouseEvent *event) override;
    mouseReleaseEvent(event)
    {
        // 原 Qt: const QPoint pos = event.pos(); rect().contains(pos) —— JZRect::contains 收两个数
        const in_rect = this.rect().contains(event.localX(), event.localY());
        if (this.m_mouseDown && in_rect)
        {
            const old_check = this.m_checked;
            // 原 QApplication::keyboardModifiers() != Qt.ControlModifier:
            // 引擎没有"全局修饰键", 用本次事件的 modifiers()(1=Shift 2=Ctrl 4=Alt)。
            if ((event.modifiers() & 2 /* JZKeyEvent.CtrlModifier */) === 0)
            {
                if (!this.m_checked)
                {
                    this.m_checked = true;
                    this.update();
                }

                if (event.button() === 1
                    || (event.button() === 2 && !old_check))
                    emit(this.sigToggled, true);   // emit toggled(true);
            }
            else
            {
                this.m_checked = !this.m_checked;
                this.update();
            }
        }

        this.m_mouseDown = false;
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

    // void setEnabled(bool flag) / bool isEnabled();
    //   引擎的 JZWidget 还没有 enabled 状态(绑定层未提供) —— 本地记账, 只影响卡片配色
    //   (见 paintEvent / bgColor)。原先靠 qtcompat 转发到真控件, 那边也没有这个语义。
    //   TODO(引擎): JZWidget 补上 setEnabled/isEnabled 后删掉这两个本地实现。
    setEnabled(flag)
    {
        if (this.m_enabled !== flag)
        {
            this.m_enabled = flag;
            this.update();
        }
    }

    isEnabled()
    {
        return this.m_enabled;
    }

    // void setSize(int itemSize);
    setSize(itemSize)
    {
        this.m_itemSize = itemSize;

        if (itemSize === Item_Normal)
        {
            this.m_font = new JZFont(12);   // 原 this.font() + setPixelSize(12)

            this.m_baseWidth = 170;
            this.m_baseHeight = 272; //增加温度显示
        }
        else if (itemSize === Item_Small)
        {
            this.m_font = new JZFont(9);   // 原 this.font() + setPixelSize(9)

            this.m_baseWidth = 120;
            this.m_baseHeight = 218; //增加温度显示
        }
        this.setScale(this.m_scale);
    }

    // QColor bgColor();
    bgColor()
    {
        if (!this.isEnabled())
            return new JZColor(170, 175, 186);

        if (this.m_status === DeviceInfo.Status_Complete)
        {
            return new JZColor(192, 192, 192);
        }
        else if (this.m_status === DeviceInfo.Status_Stop)
        {
            if (this.m_rtdata.PrtCode !== 0)
                return this.m_alarmCleared ? FaultClearedBgColor : FaultBgColor;
            else
                return new JZColor(240, 240, 240);
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
                return new JZColor(15, 175, 0);
            else if (this.m_rtdata.StepType === STEP_TYPE_2)
                return new JZColor(180, 180, 20);
            else if (this.m_rtdata.StepType === STEP_TYPE_STOP)
                return new JZColor(192, 192, 192);
            else
                return new JZColor(204, 255, 255);
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
        this.setTooltip(tips);   // 引擎的控件接口名是 setTooltip(原 Qt: setToolTip)
    }

    // void getTitle(QString &title, QColor &color);
    //   出参 -> 返回 { title, color }
    getTitle()
    {
        let title = "";
        let color = new JZColor(0, 0, 0);

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
