// JZRadioButtonQt.cpp
// 单选按钮组件的 Qt 后端实现

#include "../JZRadioButton.h"
#include <QRadioButton>
#include <QButtonGroup>
#include <QObject>
#include <unordered_map>

namespace JZ {

// 用于管理单选按钮组
static std::unordered_map<std::string, QButtonGroup*> radioGroups;

class RadioButtonQt : public RadioButton {
public:
    RadioButtonQt();
    virtual ~RadioButtonQt();

    virtual void* getNativeHandle() const override;
    virtual void updateFromProperties() override;

protected:
    QRadioButton* qtRadioButton_;
};

RadioButtonQt::RadioButtonQt() : RadioButton() {
    qtRadioButton_ = new QRadioButton();
    
    // 连接信号槽
    QObject::connect(qtRadioButton_, &QRadioButton::clicked, [this]() {
        triggerOnClick();
    });
    
    QObject::connect(qtRadioButton_, &QRadioButton::toggled, [this](bool checked) {
        checked_ = checked;
    });
}

RadioButtonQt::~RadioButtonQt() {
    if (qtRadioButton_) {
        // 从组中移除
        if (!groupName_.empty() && radioGroups.find(groupName_) != radioGroups.end()) {
            radioGroups[groupName_]->removeButton(qtRadioButton_);
            // 如果组为空，删除组
            if (radioGroups[groupName_]->buttons().isEmpty()) {
                delete radioGroups[groupName_];
                radioGroups.erase(groupName_);
            }
        }
        delete qtRadioButton_;
        qtRadioButton_ = nullptr;
    }
}

void* RadioButtonQt::getNativeHandle() const {
    return qtRadioButton_;
}

void RadioButtonQt::updateFromProperties() {
    if (!qtRadioButton_) return;
    
    // 更新基本属性
    qtRadioButton_->setGeometry(properties_.x, properties_.y, properties_.width, properties_.height);
    qtRadioButton_->setText(QString::fromStdString(properties_.text));
    qtRadioButton_->setVisible(properties_.visible);
    qtRadioButton_->setEnabled(properties_.enabled);
    
    // 更新选中状态
    qtRadioButton_->setChecked(checked_);
    
    // 更新组信息
    if (!groupName_.empty()) {
        if (radioGroups.find(groupName_) == radioGroups.end()) {
            radioGroups[groupName_] = new QButtonGroup();
        }
        radioGroups[groupName_]->addButton(qtRadioButton_);
    }
}

} // namespace JZ