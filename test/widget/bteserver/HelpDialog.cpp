#include "HelpDialog.h"

#include "JZDialog.h"
#include "JZPushButton.h"
#include "JZTextEdit.h"

HelpDialog::HelpDialog()
{
    ui.setupUi();

    if (JZTextEdit* text = (JZTextEdit*)ui.byId["textHelp"])
    {
        text->setText(
            "BTEServer(移植版)\n"
            "\n"
            "本程序是 bteserver 的 JZFramework 移植, 界面由 BTEServer.ui 生成。\n"
            "\n"
            "已搬: 设备连接 / 实时帧 / 记录点 / 日志、通道启停接续跳步、\n"
            "      查询·删除·分选、测试页收发、记录库(sqlite)、报警音。\n"
            "未搬: 工步下发与工步设置界面、分选模板界面、多语言、崩溃转储。\n");
    }

    JZDialog* dlg = (JZDialog*)ui.root;
    if (JZPushButton* ok = (JZPushButton*)ui.byId["btnOk"])
        connect(&ok->sigClicked, [dlg]() { if (dlg) dlg->accept(); });
}

HelpDialog::~HelpDialog()
{
    delete ui.root;      // 生成类建的窗口(JZDialog)不归谁管, 这里负责收尾
}

int HelpDialog::exec()
{
    JZDialog* dlg = (JZDialog*)ui.root;
    return dlg ? dlg->exec() : 0;
}

void HelpDialog::setHelpText(const std::string& text)
{
    if (JZTextEdit* t = (JZTextEdit*)ui.byId["textHelp"])
        t->setText(text);
}

std::string HelpDialog::helpText() const
{
    std::map<std::string, JZWidget*>::const_iterator it = ui.byId.find("textHelp");
    if (it != ui.byId.end())
        return ((JZTextEdit*)it->second)->text();
    return std::string();
}
