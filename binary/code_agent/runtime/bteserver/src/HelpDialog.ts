// ---------------------------------------------------------------------------
// HelpDialog.js —— 对应 C++ BTEServer/HelpDialog.h + HelpDialog.cpp
//
// 关于对话框(固定 330x220, 显示软件名/版本/版权, 链接可外开)。
// ---------------------------------------------------------------------------

import { tr } from './BTEServer.js';
import { formatDateTime, addSeconds } from './datetime.js';
import { Qt } from './qtcompat.js';

// ---------------------------------------------------------------------------
// class HelpDialog : public QDialog
// ---------------------------------------------------------------------------
export class HelpDialog
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // HelpDialog(QWidget *parent = 0);
    constructor(parent)
    {
        this.ui = new Ui_HelpDialogClass();
        this.ui.setupUi(this);
        this.setWindowFlags(this.windowFlags() & ~Qt.WindowContextHelpButtonHint);
        this.setWindowTitle(parent.windowTitle());

        this.ui.textHelp.setOpenExternalLinks(true);
        this.setMinimumSize(330, 220);	//更改固定窗口大小
        this.setMaximumSize(330, 220);	//更改固定窗口大小

        let text = tr("\n\n             电池检测系统软件            \n\n             BTEServer_V5.2.1            \n             版权所有（C）2021              \n             深圳市力仪科技有限公司               \n\n\n\n");
        text = text.replaceAll("\\n", "\n");
        /*
            let sec;
            const ret = CheckRegTime(sec);
            if (ret === SECURITY_OK)
            {
                const strDay = formatDateTime(addSeconds(new Date(), sec), "yyyy-MM-dd");
                text += tr("已注册,到期时间:") + strDay;
            }
            else
                text += tr("未注册");
        */
        this.ui.textHelp.setText(text);
    }

    // void on_textHelp_anchorClicked(const QUrl & link);
    on_textHelp_anchorClicked(link)
    {
        QDesktopServices.openUrl(link);
    }
}
