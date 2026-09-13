// ---------------------------------------------------------------------------
// DeviceMacDialog.js —— 对应 C++ BTEServer/DeviceMacDialog.h + DeviceMacDialog.cpp
//
// 写中位机号与 MAC 号的小对话框(无边框校验整数 1..255)。
// ---------------------------------------------------------------------------

import { tr } from './BTEServer.js';

// ---------------------------------------------------------------------------
// class DeviceMacDialog : public QDialog
// ---------------------------------------------------------------------------
export class DeviceMacDialog
{
    // DeviceMacDialog(QWidget *parent = Q_NULLPTR);
    constructor(parent)
    {
        this.mac = 0;   // int mac;
        this.dev = 0;   // int dev;

        this.ui = new Ui_DeviceMacDialogClass();
        this.ui.setupUi(this);
        this.dev = this.mac = 0;

        const l = new QIntValidator(this);
        l.setRange(1, 255);

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
