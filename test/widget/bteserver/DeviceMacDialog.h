#ifndef BTE_DEVICE_MAC_DIALOG_H_
#define BTE_DEVICE_MAC_DIALOG_H_

// ---------------------------------------------------------------------------
// DeviceMacDialog —— 对应原版 bteserver/BTEServer/DeviceMacDialog.{h,cpp}
// (设备列表右键菜单"更改中位机IP": 设备号 + 中位机号两个 1..255 的数字)。
//
// 形状差异同 HelpDialog: 生成类自己 new JZDialog, 本类是宿主, 对外给 exec();
// 原版两个 LineEdit 上的 QIntValidator(1..255) 引擎没有 ⇒ 在 exec() 返回前自己夹范围。
// ---------------------------------------------------------------------------

#include <string>
#include "ui_DeviceMacDialog.h"

class DeviceMacDialog
{
public:
    DeviceMacDialog();
    ~DeviceMacDialog();                // 生成类自己 new 的窗口要手工删

    int  dev = 0;      // 设备号
    int  mac = 0;      // 中位机号

    int exec();        // Accepted/Rejected(见 JZDialog::Result)

private:
    Ui::DeviceMacDialogUi ui;
};

#endif // BTE_DEVICE_MAC_DIALOG_H_
