// JZHBoxLayout.h
// 水平布局组件定义

#ifndef JZ_HBOX_LAYOUT_H
#define JZ_HBOX_LAYOUT_H

#include "JZLayout.h"

namespace JZ {

class HBoxLayout : public Layout {
public:
    HBoxLayout();
    virtual ~HBoxLayout();

    // 重写添加组件方法
    virtual void addWidget(std::shared_ptr<Widget> widget, int stretch = 0) override;
    
    // 重写后端实现接口
    virtual void* getNativeHandle() const override;
    virtual void updateLayout() override;
};

} // namespace JZ

#endif // JZ_HBOX_LAYOUT_H