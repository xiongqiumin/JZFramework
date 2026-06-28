// JZWidget.h
// 基础组件类定义

#ifndef JZWIDGET_H
#define JZWIDGET_H

#include <string>
#include <memory>
#include <functional>

namespace JZ {

// 组件属性结构体
enum class WidgetType {
    Base,
    PushButton,
    Label,
    CheckBox,
    RadioButton,
    ComboBox,
    TextEdit,
    TabWidget
};

struct WidgetProperties {
    std::string id;
    std::string text;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool visible = true;
    bool enabled = true;
};

// 基础组件类
class Widget {
public:
    Widget(WidgetType type = WidgetType::Base);
    virtual ~Widget();

    // 获取组件类型
    WidgetType getType() const;

    // 设置和获取ID
    void setId(const std::string& id);
    std::string getId() const;

    // 设置和获取文本
    void setText(const std::string& text);
    std::string getText() const;

    // 设置和获取位置
    void setPosition(int x, int y);
    void getPosition(int& x, int& y) const;

    // 设置和获取尺寸
    void setSize(int width, int height);
    void getSize(int& width, int& height) const;

    // 设置和获取可见性
    void setVisible(bool visible);
    bool isVisible() const;

    // 设置和获取启用状态
    void setEnabled(bool enabled);
    bool isEnabled() const;

    // 事件回调
    using EventCallback = std::function<void()>;
    void setOnClickCallback(const EventCallback& callback);
    void setOnFocusCallback(const EventCallback& callback);
    void setOnLostFocusCallback(const EventCallback& callback);

    // 触发事件（由后端调用）
    void triggerOnClick();
    void triggerOnFocus();
    void triggerOnLostFocus();

    // 后端实现接口
    virtual void* getNativeHandle() const = 0;
    virtual void updateFromProperties() = 0;

protected:
    WidgetType type_;
    WidgetProperties properties_;
    EventCallback onClickCallback_;
    EventCallback onFocusCallback_;
    EventCallback onLostFocusCallback_;
};

// 工厂方法，创建具体的组件
std::shared_ptr<Widget> createWidget(WidgetType type);

} // namespace JZ

#endif // JZWIDGET_H