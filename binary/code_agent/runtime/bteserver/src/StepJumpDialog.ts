// ---------------------------------------------------------------------------
// StepJumpDialog.js —— 对应 C++ BTEServer/StepJumpDialog.h + StepJumpDialog.cpp
//
// 工步跳转对话框: 用 StepTableHelper 把当前工步表铺到 tableWidget, 选中的行号即跳转目标。
// ---------------------------------------------------------------------------

import { StepTableHelper } from './StepSettingDialog.js';
import { STEPJUMPDIALOG_UI_XML } from './ui/STEPJUMPDIALOG_UI_XML.js';

// ---------------------------------------------------------------------------
// class StepJumpDialog : public QDialog
// ---------------------------------------------------------------------------
export class StepJumpDialog
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // StepJumpDialog(QWidget *parent = Q_NULLPTR);
    constructor(parent)
    {
        // 真 UI: 原版 StepJumpDialog.ui 转换出的 XML; root 是**新窗口**(JZDialog), 不是 this。
        this.ui = uiLoader.load(STEPJUMPDIALOG_UI_XML);
        // parent 传进来的是"持真窗口的 JS 对象"(如 BTEServer), 真控件在它的 .ui 上; 取对了再挂。
        if (parent) { const pw = parent.ui ? parent.ui : parent; if (pw) this.ui.setParentWidget(pw); }

        this.m_tableHelper = new StepTableHelper();
        this.m_tableHelper.init(this.ui.tableWidget);
        this.select = 0;   // int select;
    }

    // ~StepJumpDialog();
    dispose()
    {
    }

    // virtual void showEvent(QShowEvent *event) override;
    showEvent(event)
    {
        this.updateTable();
        QDialog.prototype.showEvent.call(this, event);   // __super::showEvent(event);
    }

    // void updateTable();
    updateTable()
    {
        this.m_tableHelper.updateTable(this.stepInfo);
        this.ui.tableWidget.setCurrentItem(this.ui.tableWidget.item(0, 0));
    }

    // void on_btnOk_clicked();
    on_btnOk_clicked()
    {
        this.select = this.ui.tableWidget.currentRow();
        this.accept();
    }

    // void on_btnCancel_clicked();
    on_btnCancel_clicked()
    {
        this.reject();
    }
}
