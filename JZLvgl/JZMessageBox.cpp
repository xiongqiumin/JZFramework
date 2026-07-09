#include "JZMessageBox.h"
#include "JZLabel.h"
#include "JZPushButton.h"
#include "JZLayout.h"
#include "JZGuiApplication.h"

JZ_OBJECT_IMPL(JZMessageBox, JZDialog)

JZMessageBox::JZMessageBox(JZWidget* parent, const std::string& title,
                           const std::string& text, bool hasCancel)
    : JZDialog(parent)
{
    resize(300, 150);

    // 文本
    auto* textLabel = new JZLabel(this);
    textLabel->setText(text);
    textLabel->move(20, 30);
    textLabel->resize(260, 40);

    // 按钮区
    auto* btnRow = new JZWidget(this);
    btnRow->resize(160, 35);
    btnRow->move(70, 90);
    btnRow->setLayout(new JZHBoxLayout(4, 8));

    if (hasCancel) {
        auto* cancelBtn = new JZPushButton(btnRow);
        cancelBtn->resize(75, 30);
        cancelBtn->setText("Cancel");
        connect(&cancelBtn->sigClicked, this, &JZDialog::reject);
    }

    auto* okBtn = new JZPushButton(btnRow);
    okBtn->resize(75, 30);
    okBtn->setText("OK");
    connect(&okBtn->sigClicked, this, &JZDialog::accept);
}

JZMessageBox* JZMessageBox::information(JZWidget* parent,
                                         const std::string& title, const std::string& text)
{
    return new JZMessageBox(parent, title, text, false);
}

JZMessageBox* JZMessageBox::warning(JZWidget* parent,
                                     const std::string& title, const std::string& text)
{
    return new JZMessageBox(parent, title, text, false);
}

JZMessageBox* JZMessageBox::question(JZWidget* parent,
                                      const std::string& title, const std::string& text)
{
    return new JZMessageBox(parent, title, text, true);
}
