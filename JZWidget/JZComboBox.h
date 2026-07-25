#ifndef JZ_WIDGET_DROPDOWN_H_
#define JZ_WIDGET_DROPDOWN_H_

#include "JZWidget.h"
#include "JZSignal.h"
#include <vector>
#include <string>

class JZComboBox : public JZWidget
{
    JZ_OBJECT_DECLARE(JZComboBox)

public:
    explicit JZComboBox(JZWidget* parent = nullptr);

    JZSize sizeHint() const override;

    // 添加项
    void addItem(const std::string& text);
    void addItems(const std::vector<std::string>& texts);
    void insertItem(int index, const std::string& text);

    // 移除项
    void removeItem(int index);
    void clear();

    // 当前选中
    int currentIndex() const;
    void setCurrentIndex(int index);
    std::string currentText() const;
    void setCurrentText(const std::string& text);

    // 项信息
    std::string itemText(int index) const;
    int count() const;

jz_signals:
    JZSigInt currentIndexChanged;   // 当前选中索引变化
    JZSigString currentTextChanged;  // 当前选中文本变化

protected:
};

#endif
