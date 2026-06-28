// JZTextEdit.h
// 文本编辑框组件定义

#ifndef JZ_TEXT_EDIT_H
#define JZ_TEXT_EDIT_H

#include "JZWidget.h"

namespace JZ {

class TextEdit : public Widget {
public:
    TextEdit();
    virtual ~TextEdit();

    // 文本编辑框特有方法
    void setText(const std::string& text);
    std::string getText() const;

    void setPlaceholderText(const std::string& placeholder);
    std::string getPlaceholderText() const;

    void setReadOnly(bool readOnly);
    bool isReadOnly() const;

    void setMaxLength(int maxLength);
    int getMaxLength() const;

    void setEchoMode(int echoMode); // 0: 正常, 1: 密码
    int getEchoMode() const;

    // 重写后端实现接口
    virtual void* getNativeHandle() const override;
    virtual void updateFromProperties() override;

protected:
    std::string placeholderText_;
    bool readOnly_ = false;
    int maxLength_ = -1; // -1 表示无限制
    int echoMode_ = 0;   // 0: 正常, 1: 密码
};

} // namespace JZ

#endif // JZ_TEXT_EDIT_H