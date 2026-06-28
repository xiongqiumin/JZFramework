// JZComboBox.h
// 下拉框组件定义

#ifndef JZ_COMBO_BOX_H
#define JZ_COMBO_BOX_H

#include "JZWidget.h"
#include <vector>

namespace JZ {

class ComboBox : public Widget {
public:
    ComboBox();
    virtual ~ComboBox();

    // 下拉框特有方法
    void addItem(const std::string& text);
    void addItems(const std::vector<std::string>& items);
    void removeItem(int index);
    void clearItems();
    
    void setCurrentIndex(int index);
    int getCurrentIndex() const;
    
    std::string getCurrentText() const;
    std::vector<std::string> getAllItems() const;

    // 重写后端实现接口
    virtual void* getNativeHandle() const override;
    virtual void updateFromProperties() override;

protected:
    std::vector<std::string> items_;
    int currentIndex_ = -1;
};

} // namespace JZ

#endif // JZ_COMBO_BOX_H