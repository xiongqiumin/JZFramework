// JZPushButtonQt.cpp
// 按钮组件的 Qt 后端实现

#include "../JZPushButton.h"
#include <QPushButton>
#include <QObject>

namespace JZ {

class PushButtonQt : public PushButton {
public:
    PushButtonQt();
    virtual ~PushButtonQt();

    virtual void* getNativeHandle() const override;
    virtual void updateFromProperties() override;

protected:
    QPushButton* qtButton_;
};

PushButtonQt::PushButtonQt() : PushButton() {
    qtButton_ = new QPushButton();
    
    // 连接信号槽
    QObject::connect(qtButton_, &QPushButton::clicked, [this]() {
        triggerOnClick();
    });
    
    QObject::connect(qtButton_, &QPushButton::toggled, [this](bool checked) {
        checked_ = checked;
    });
}

PushButtonQt::~PushButtonQt() {
    if (qtButton_) {
        delete qtButton_;
        qtButton_ = nullptr;
    }
}

void* PushButtonQt::getNativeHandle() const {
    return qtButton_;
}

void PushButtonQt::updateFromProperties() {
    if (!qtButton_) return;
    
    // 更新基本属性
    qtButton_->setGeometry(properties_.x, properties_.y, properties_.width, properties_.height);
    qtButton_->setText(QString::fromStdString(properties_.text));
    qtButton_->setVisible(properties_.visible);
    qtButton_->setEnabled(properties_.enabled);
    
    // 更新按钮特有属性
    qtButton_->setCheckable(checkable_);
    qtButton_->setChecked(checked_);
}

} // namespace JZ