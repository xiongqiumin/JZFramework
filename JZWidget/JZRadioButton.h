// JZRadioButton.h
// 单选按钮组件定义

#ifndef JZ_RADIO_BUTTON_H
#define JZ_RADIO_BUTTON_H

#include "JZWidget.h"

namespace JZ {

class RadioButton : public Widget {
public:
    RadioButton();
    virtual ~RadioButton();

    // 单选按钮特有方法
    void setChecked(bool checked);
    bool isChecked() const;

    void setGroupName(const std::string& groupName);
    std::string getGroupName() const;

    // 重写后端实现接口
    virtual void* getNativeHandle() const override;
    virtual void updateFromProperties() override;

protected:
    bool checked_ = false;
    std::string groupName_;
};

} // namespace JZ

#endif // JZ_RADIO_BUTTON_H