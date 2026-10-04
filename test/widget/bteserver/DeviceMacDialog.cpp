#include "DeviceMacDialog.h"

#include "JZDialog.h"
#include "JZLineEdit.h"
#include "JZPushButton.h"

#include <cstdlib>

namespace {

// 原版 QIntValidator(1..255): 引擎的 JZLineEdit 没有 validator ⇒ 取值时夹一下
int clamp255(const std::string& text)
{
    const int v = atoi(text.c_str());
    if (v < 1)
        return 1;
    if (v > 255)
        return 255;
    return v;
}

} // namespace

DeviceMacDialog::DeviceMacDialog()
{
    ui.setupUi();

    JZDialog* dlg = (JZDialog*)ui.root;

    if (JZLineEdit* lineDev = (JZLineEdit*)ui.byId["lineDevice"])
        lineDev->setText(std::to_string(dev));
    if (JZLineEdit* lineMac = (JZLineEdit*)ui.byId["lineMac"])
        lineMac->setText(std::to_string(mac));

    if (JZPushButton* ok = (JZPushButton*)ui.byId["btnOk"])
    {
        connect(&ok->sigClicked, [this, dlg]() {
            if (JZLineEdit* lineDev = (JZLineEdit*)ui.byId["lineDevice"])
                dev = clamp255(lineDev->text());
            if (JZLineEdit* lineMac = (JZLineEdit*)ui.byId["lineMac"])
                mac = clamp255(lineMac->text());
            if (dlg)
                dlg->accept();
        });
    }
    if (JZPushButton* cancel = (JZPushButton*)ui.byId["btnCancel"])
        connect(&cancel->sigClicked, [dlg]() { if (dlg) dlg->reject(); });
}

DeviceMacDialog::~DeviceMacDialog()
{
    delete ui.root;      // 生成类建的窗口(JZDialog)不归谁管, 这里负责收尾
}

int DeviceMacDialog::exec()
{
    // 原版 showEvent 里把当前值填进输入框; 这里每次弹之前刷一次
    if (JZLineEdit* lineDev = (JZLineEdit*)ui.byId["lineDevice"])
        lineDev->setText(std::to_string(dev));
    if (JZLineEdit* lineMac = (JZLineEdit*)ui.byId["lineMac"])
        lineMac->setText(std::to_string(mac));

    JZDialog* dlg = (JZDialog*)ui.root;
    return dlg ? dlg->exec() : 0;
}
