// JZWidgetQt.cpp
// Qt 后端组件实现

#include "../JZWidget.h"
#include <QWidget>
#include <QObject>

namespace JZ {

// 基础组件的 Qt 实现类
class WidgetQt : public Widget {
public:
    WidgetQt(WidgetType type);
    virtual ~WidgetQt();

    virtual void* getNativeHandle() const override;
    virtual void updateFromProperties() override;

protected:
    QWidget* nativeWidget_;
};

WidgetQt::WidgetQt(WidgetType type) : Widget(type), nativeWidget_(nullptr) {
    // 创建 Qt 基础组件
    nativeWidget_ = new QWidget();
    
    // 连接信号槽
    QObject::connect(nativeWidget_, &QWidget::clicked, [this]() {
        triggerOnClick();
    });
    
    QObject::connect(nativeWidget_, &QWidget::focusInEvent, [this]() {
        triggerOnFocus();
    });
    
    QObject::connect(nativeWidget_, &QWidget::focusOutEvent, [this]() {
        triggerOnLostFocus();
    });
}

WidgetQt::~WidgetQt() {
    if (nativeWidget_) {
        delete nativeWidget_;
        nativeWidget_ = nullptr;
    }
}

void* WidgetQt::getNativeHandle() const {
    return nativeWidget_;
}

void WidgetQt::updateFromProperties() {
    if (!nativeWidget_) return;
    
    // 更新位置和大小
    nativeWidget_->setGeometry(properties_.x, properties_.y, properties_.width, properties_.height);
    
    // 更新可见性
    nativeWidget_->setVisible(properties_.visible);
    
    // 更新启用状态
    nativeWidget_->setEnabled(properties_.enabled);
    
    // 更新文本
    nativeWidget_->setWindowTitle(QString::fromStdString(properties_.text));
}

// 工厂方法实现
std::shared_ptr<Widget> createWidget(WidgetType type) {
    // 这里需要根据不同类型创建具体的组件
    // 后续会实现各个具体组件的 Qt 后端
    return std::make_shared<WidgetQt>(type);
}

} // namespace JZ