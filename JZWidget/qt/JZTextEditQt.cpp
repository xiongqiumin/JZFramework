// JZTextEditQt.cpp
// 文本编辑框组件的 Qt 后端实现

#include "../JZTextEdit.h"
#include <QLineEdit>
#include <QObject>

namespace JZ {

class TextEditQt : public TextEdit {
public:
    TextEditQt();
    virtual ~TextEditQt();

    virtual void* getNativeHandle() const override;
    virtual void updateFromProperties() override;

protected:
    QLineEdit* qtLineEdit_;
};

TextEditQt::TextEditQt() : TextEdit() {
    qtLineEdit_ = new QLineEdit();
    
    // 连接信号槽
    QObject::connect(qtLineEdit_, &QLineEdit::textChanged, [this](const QString& text) {
        properties_.text = text.toStdString();
    });
}

TextEditQt::~TextEditQt() {
    if (qtLineEdit_) {
        delete qtLineEdit_;
        qtLineEdit_ = nullptr;
    }
}

void* TextEditQt::getNativeHandle() const {
    return qtLineEdit_;
}

void TextEditQt::updateFromProperties() {
    if (!qtLineEdit_) return;
    
    // 更新基本属性
    qtLineEdit_->setGeometry(properties_.x, properties_.y, properties_.width, properties_.height);
    qtLineEdit_->setText(QString::fromStdString(properties_.text));
    qtLineEdit_->setVisible(properties_.visible);
    qtLineEdit_->setEnabled(properties_.enabled);
    
    // 更新占位符文本
    if (!placeholderText_.empty()) {
        qtLineEdit_->setPlaceholderText(QString::fromStdString(placeholderText_));
    }
    
    // 更新只读状态
    qtLineEdit_->setReadOnly(readOnly_);
    
    // 更新最大长度
    if (maxLength_ >= 0) {
        qtLineEdit_->setMaxLength(maxLength_);
    }
    
    // 更新回显模式
    switch (echoMode_) {
        case 1: // 密码
            qtLineEdit_->setEchoMode(QLineEdit::Password);
            break;
        default: // 正常
            qtLineEdit_->setEchoMode(QLineEdit::Normal);
            break;
    }
}

} // namespace JZ