// JZLayout.h
// 布局基类定义

#ifndef JZ_LAYOUT_H
#define JZ_LAYOUT_H

#include "JZWidget.h"
#include <vector>

namespace JZ {

enum class LayoutType {
    Base,
    HBox,
    VBox,
    Grid
};

class Layout {
public:
    Layout(LayoutType type = LayoutType::Base);
    virtual ~Layout();

    // 获取布局类型
    LayoutType getType() const;

    // 添加和移除组件
    virtual void addWidget(std::shared_ptr<Widget> widget, int stretch = 0);
    virtual void removeWidget(std::shared_ptr<Widget> widget);
    virtual void clear();

    // 设置边距
    void setMargins(int left, int top, int right, int bottom);
    void getMargins(int& left, int& top, int& right, int& bottom) const;

    // 设置间距
    void setSpacing(int spacing);
    int getSpacing() const;

    // 后端实现接口
    virtual void* getNativeHandle() const = 0;
    virtual void updateLayout() = 0;

protected:
    LayoutType type_;
    std::vector<std::shared_ptr<Widget>> widgets_;
    std::vector<int> stretchFactors_;
    int margins_[4] = {0, 0, 0, 0}; // left, top, right, bottom
    int spacing_ = 0;
};

} // namespace JZ

#endif // JZ_LAYOUT_H