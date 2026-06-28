// JZCheckBoxQt.cpp
// 复选框组件的 Qt 后端实现

#include "../JZCheckBox.h"
#include <QCheckBox>
#include <QObject>

namespace JZ {

class CheckBoxQt : public CheckBox {
public:
    CheckBoxQt();
    virtual ~CheckBoxQt();

    virtual void* getNativeHandle() const override;
    virtual void updateFromProperties() override;

protected:
    QCheckBox* qtCheckBox_;
};

CheckBoxQt::CheckBoxQt() : CheckBox() {
    qtCheckBox_ = new QCheckBox();
    
    // 连接信号槽
    QObject::connect(qtCheckBox_, &QCheckBox::clicked, [this]() {
        triggerOnClick();
    });
    
    QObject::connect(qtCheckBox_, &QCheckBox::toggled, [this](bool checked) {
        checked_ = checked;
    });
}

CheckBoxQt::~CheckBoxQt() {
    if (qtCheckBox_) {
        delete qtCheckBox_;
        qtCheckBox_ = nullptr;
    }
}

void* CheckBoxQt::getNativeHandle() const {
    return qtCheckBox_;
}

void CheckBoxQt::updateFromProperties() {
    if (!qtCheckBox_) return;
    
    // 更新基本属性
    qtCheckBox_->setGeometry(properties_.x, properties_.y, properties_.width, properties_.height);
    qtCheckBox_->setText(QString::fromStdString(properties_.text));
    qtCheckBox_->setVisible(properties_.visible);
    qtCheckBox_->setEnabled(properties_.enabled);
    
    // 更新选中状态
    qtCheckBox_->setChecked(checked_);
}

} // namespace JZ