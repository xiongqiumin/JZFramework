// JZLabelQt.cpp
// 标签组件的 Qt 后端实现

#include "../JZLabel.h"
#include <QLabel>
#include <QFont>

namespace JZ {

class LabelQt : public Label {
public:
    LabelQt();
    virtual ~LabelQt();

    virtual void* getNativeHandle() const override;
    virtual void updateFromProperties() override;

protected:
    QLabel* qtLabel_;
};

LabelQt::LabelQt() : Label() {
    qtLabel_ = new QLabel();
}

LabelQt::~LabelQt() {
    if (qtLabel_) {
        delete qtLabel_;
        qtLabel_ = nullptr;
    }
}

void* LabelQt::getNativeHandle() const {
    return qtLabel_;
}

void LabelQt::updateFromProperties() {
    if (!qtLabel_) return;
    
    // 更新基本属性
    qtLabel_->setGeometry(properties_.x, properties_.y, properties_.width, properties_.height);
    qtLabel_->setText(QString::fromStdString(properties_.text));
    qtLabel_->setVisible(properties_.visible);
    
    // 更新对齐方式
    Qt::Alignment align;
    switch (alignment_) {
        case 1: // 居中
            align = Qt::AlignCenter;
            break;
        case 2: // 右对齐
            align = Qt::AlignRight | Qt::AlignVCenter;
            break;
        default: // 左对齐
            align = Qt::AlignLeft | Qt::AlignVCenter;
            break;
    }
    qtLabel_->setAlignment(align);
    
    // 更新文本颜色
    if (!textColor_.empty()) {
        QPalette palette = qtLabel_->palette();
        palette.setColor(QPalette::WindowText, QColor(QString::fromStdString(textColor_)));
        qtLabel_->setPalette(palette);
    }
}

} // namespace JZ