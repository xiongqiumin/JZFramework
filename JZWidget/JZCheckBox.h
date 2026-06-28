// JZCheckBox.h
// 复选框组件定义

#ifndef JZ_CHECK_BOX_H
#define JZ_CHECK_BOX_H

#include "JZWidget.h"

namespace JZ {

class CheckBox : public Widget {
public:
    CheckBox();
    virtual ~CheckBox();

    // 复选框特有方法
    void setChecked(bool checked);
    bool isChecked() const;

    // 重写后端实现接口
    virtual void* getNativeHandle() const override;
    virtual void updateFromProperties() override;

protected:
    bool checked_ = false;
};

} // namespace JZ

#endif // JZ_CHECK_BOX_H