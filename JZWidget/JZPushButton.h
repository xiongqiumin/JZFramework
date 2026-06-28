// JZPushButton.h
// 按钮组件定义

#ifndef JZ_PUSH_BUTTON_H
#define JZ_PUSH_BUTTON_H

#include "JZWidget.h"

namespace JZ {

class PushButton : public Widget {
public:
    PushButton();
    virtual ~PushButton();

    // 按钮特有方法
    void setCheckable(bool checkable);
    bool isCheckable() const;

    void setChecked(bool checked);
    bool isChecked() const;

    // 重写后端实现接口
    virtual void* getNativeHandle() const override;
    virtual void updateFromProperties() override;

protected:
    bool checkable_ = false;
    bool checked_ = false;
};

} // namespace JZ

#endif // JZ_PUSH_BUTTON_H