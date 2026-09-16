#include "JZMessageBox.h"
#include "JZLabel.h"
#include "JZPushButton.h"
#include "JZLayout.h"

JZ_OBJECT_IMPL(JZMessageBox, JZDialog)

JZMessageBox::JZMessageBox(JZWidget* parent, const std::string& title,
                           const std::string& text, bool hasCancel)
    : JZDialog(parent)
{
    setTitle(title);

    // 整体纵向布局: 文本 + 按钮行
    auto* vbox = new JZVBoxLayout(16, 12);
    setLayout(vbox);

    // 文本
    auto* textLabel = new JZLabel(this);
    textLabel->setText(text);
    vbox->addWidget(textLabel, 0);

    // 按钮行: 右侧对齐 (stretch 占位)
    JZWidget* btnRow = new JZWidget(this);
    JZHBoxLayout* bh = new JZHBoxLayout(8, 8);
    bh->addStretch(1);
    auto* okBtn = new JZPushButton("OK", btnRow);
    bh->addWidget(okBtn, 0);
    connect(&okBtn->sigClicked, this, &JZDialog::accept);
    if (hasCancel)
    {
        auto* cancelBtn = new JZPushButton("Cancel", btnRow);
        bh->addWidget(cancelBtn, 0);
        connect(&cancelBtn->sigClicked, this, &JZDialog::reject);
    }
    btnRow->setLayout(bh);
    vbox->addWidget(btnRow, 0);
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
