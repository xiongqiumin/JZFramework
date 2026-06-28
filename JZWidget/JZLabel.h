// JZLabel.h
// 标签组件定义

#ifndef JZ_LABEL_H
#define JZ_LABEL_H

#include "JZWidget.h"

namespace JZ {

class Label : public Widget {
public:
    Label();
    virtual ~Label();

    // 标签特有方法
    void setAlignment(int alignment);
    int getAlignment() const;

    void setTextColor(const std::string& color);
    std::string getTextColor() const;

    // 重写后端实现接口
    virtual void* getNativeHandle() const override;
    virtual void updateFromProperties() override;

protected:
    int alignment_ = 0; // 0: 左对齐, 1: 居中, 2: 右对齐
    std::string textColor_;
};

} // namespace JZ

#endif // JZ_LABEL_H