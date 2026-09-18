// ---------------------------------------------------------------------------
// HelpDialog.js —— 对应 C++ BTEServer/HelpDialog.h + HelpDialog.cpp
//
// 关于对话框(固定 330x220, 显示软件名/版本/版权, 链接可外开)。
// ---------------------------------------------------------------------------

import { tr } from './BTEServer.js';
import { formatDateTime, addSeconds } from './datetime.js';
import { HELPDIALOG_UI_XML } from './ui/HELPDIALOG_UI_XML.js';

// ---------------------------------------------------------------------------
// class HelpDialog : public QDialog
// ---------------------------------------------------------------------------
export class HelpDialog
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // HelpDialog(QWidget *parent = 0);
    constructor(parent)
    {
        // 真 UI: 原版 HelpDialog.ui 转换出的 XML。root 是**新窗口**(JZDialog), 不是 this。
        this.ui = uiLoader.load(HELPDIALOG_UI_XML);
        // parent 传进来的是"持真窗口的 JS 对象"(如 BTEServer), 真控件在它的 .ui 上; 取对了再挂。
        if (parent) { const pw = parent.ui ? parent.ui : parent; if (pw) this.ui.setParentWidget(pw); }
        // 原: setWindowFlags(windowFlags() & ~Qt.WindowContextHelpButtonHint) ——
        //     引擎的窗口没有 Qt 那套 windowFlags(标题栏按钮由框架统一给), 这行去掉。
        this.ui.setTitle(parent ? parent.title() : "帮助");   // 原为 setWindowTitle(parent.windowTitle())

        // 原: this.ui.textHelp.setOpenExternalLinks(true) —— 引擎的 TextEdit 不解析富文本/链接, 去掉。
        // 原: setMinimumSize/setMaximumSize(330,220) —— 尺寸已放进 .ui 的 geometry(330x220), 不再单独设。

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
        JZDesktopServices.openUrl(link);   // 引擎侧门面: JZDesktopServices.openUrl
    }
}
