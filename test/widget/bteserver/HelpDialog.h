#ifndef BTE_HELP_DIALOG_H_
#define BTE_HELP_DIALOG_H_

// ---------------------------------------------------------------------------
// HelpDialog —— 对应原版 bteserver/BTEServer/HelpDialog.{h,cpp}(菜单"帮助-关于")。
//
// ⚠ 形状差异: 原版 `class HelpDialog : public QDialog`, 自己就是窗口; 这里引擎的 .ui
// 生成类(Ui::HelpDialogUi::setupUi)**自己 new 出 JZDialog**, 所以本类是"持有生成类的
// 宿主"(与 BTEServer 同样的做法), 对外给出 exec() —— 调用方看到的还是"弹个框"。
//
// 原版还接了 on_textHelp_anchorClicked(点链接跳官网), 那是富文本能力, 引擎没有 ⇒ 不搬。
// ---------------------------------------------------------------------------

#include <string>
#include "ui_HelpDialog.h"

class HelpDialog
{
public:
    HelpDialog();
    ~HelpDialog();                            // 生成类自己 new 的窗口要手工删(原版没有这层)

    int  exec();                              // 模态(原版 QDialog::exec)
    void setHelpText(const std::string& text);
    std::string helpText() const;

private:
    Ui::HelpDialogUi ui;
};

#endif // BTE_HELP_DIALOG_H_
