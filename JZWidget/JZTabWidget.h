// JZTabWidget.h
// 标签页组件定义

#ifndef JZ_TAB_WIDGET_H
#define JZ_TAB_WIDGET_H

#include "JZWidget.h"
#include <vector>
#include <string>

namespace JZ {

class TabWidget : public Widget {
public:
    TabWidget();
    virtual ~TabWidget();

    // 标签页特有方法
    int addTab(const std::string& title, std::shared_ptr<Widget> widget);
    void removeTab(int index);
    void clearTabs();
    
    void setCurrentIndex(int index);
    int getCurrentIndex() const;
    
    std::string getTabText(int index) const;
    void setTabText(int index, const std::string& text);
    
    int count() const;

    // 重写后端实现接口
    virtual void* getNativeHandle() const override;
    virtual void updateFromProperties() override;

protected:
    struct TabInfo {
        std::string title;
        std::shared_ptr<Widget> widget;
    };
    
    std::vector<TabInfo> tabs_;
    int currentIndex_ = -1;
};

} // namespace JZ

#endif // JZ_TAB_WIDGET_H