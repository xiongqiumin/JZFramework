// ---------------------------------------------------------------------------
// StepJumpDialog.js —— 对应 C++ BTEServer/StepJumpDialog.h + StepJumpDialog.cpp
//
// 工步跳转对话框: 用 StepTableHelper 把当前工步表铺到 tableWidget, 选中的行号即跳转目标。
// ---------------------------------------------------------------------------

import { StepTableHelper } from './StepSettingDialog.js';

// ---------------------------------------------------------------------------
// class StepJumpDialog : public QDialog
// ---------------------------------------------------------------------------
export class StepJumpDialog
{
    // StepJumpDialog(QWidget *parent = Q_NULLPTR);
    constructor(parent)
    {
        this.ui = new Ui_StepJumpDialogClass();
        this.ui.setupUi(this);

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
