// ---------------------------------------------------------------------------
// DeviceMacDialog.js —— 对应 C++ BTEServer/DeviceMacDialog.h + DeviceMacDialog.cpp
//
// 写中位机号与 MAC 号的小对话框(无边框校验整数 1..255)。
// ---------------------------------------------------------------------------

import { tr } from './BTEServer.js';
import { DEVICEMACDIALOG_UI_XML } from './ui/DEVICEMACDIALOG_UI_XML.js';

// ---------------------------------------------------------------------------
// class DeviceMacDialog : public QDialog
// ---------------------------------------------------------------------------
export class DeviceMacDialog
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // DeviceMacDialog(QWidget *parent = Q_NULLPTR);
    constructor(parent)
    {
        this.mac = 0;   // int mac;
        this.dev = 0;   // int dev;

        // 真 UI: 原版 DeviceMacDialog.ui 转换出的 XML。root 是**新窗口**(JZDialog), 不是 this
        // —— 与 uic 的 `setupUi(this)` 不同, 下面统一用 this.ui.<控件> / this.ui.<窗口方法>。
        this.ui = uiLoader.load(DEVICEMACDIALOG_UI_XML);
        // parent 传进来的是"持真窗口的 JS 对象"(如 BTEServer), 真控件在它的 .ui 上; 取对了再挂。
        if (parent) { const pw = parent.ui ? parent.ui : parent; if (pw) this.ui.setParentWidget(pw); }
        this.dev = this.mac = 0;

        // 原: QIntValidator(1..255) —— 引擎没有输入校验器(控件只存文本), 取值范围的判断留给提交处。
        // const l = new QIntValidator(this); l.setRange(1, 255);

        this.ui.lineDevice.setValidator(l);
        this.ui.lineMac.setValidator(l);
    }

    // ~DeviceMacDialog();
    dispose()
    {
    }

    // void showEvent(QShowEvent *event);
    showEvent(event)
    {
        this.ui.lineDevice.setText(String(this.dev));
        this.ui.lineMac.setText(String(this.mac));
    }

    // void on_btnOk_clicked();
    on_btnOk_clicked()
    {
        this.dev = parseInt(this.ui.lineDevice.text(), 10);
        this.mac = parseInt(this.ui.lineMac.text(), 10);
        this.accept();
    }

    // void on_btnCancel_clicked();
    on_btnCancel_clicked()
    {
        this.reject();
    }
}
