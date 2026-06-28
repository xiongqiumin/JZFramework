// JZComboBoxQt.cpp
// 下拉框组件的 Qt 后端实现

#include "../JZComboBox.h"
#include <QComboBox>
#include <QObject>

namespace JZ {

class ComboBoxQt : public ComboBox {
public:
    ComboBoxQt();
    virtual ~ComboBoxQt();

    virtual void* getNativeHandle() const override;
    virtual void updateFromProperties() override;

protected:
    QComboBox* qtComboBox_;
};

ComboBoxQt::ComboBoxQt() : ComboBox() {
    qtComboBox_ = new QComboBox();
    
    // 连接信号槽
    QObject::connect(qtComboBox_, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
        currentIndex_ = index;
    });
}

ComboBoxQt::~ComboBoxQt() {
    if (qtComboBox_) {
        delete qtComboBox_;
        qtComboBox_ = nullptr;
    }
}

void* ComboBoxQt::getNativeHandle() const {
    return qtComboBox_;
}

void ComboBoxQt::updateFromProperties() {
    if (!qtComboBox_) return;
    
    // 更新基本属性
    qtComboBox_->setGeometry(properties_.x, properties_.y, properties_.width, properties_.height);
    qtComboBox_->setVisible(properties_.visible);
    qtComboBox_->setEnabled(properties_.enabled);
    
    // 更新选项
    qtComboBox_->clear();
    for (const auto& item : items_) {
        qtComboBox_->addItem(QString::fromStdString(item));
    }
    
    // 更新当前索引
    if (currentIndex_ >= 0 && currentIndex_ < qtComboBox_->count()) {
        qtComboBox_->setCurrentIndex(currentIndex_);
    }
}

} // namespace JZ