// JZGridLayout.h
// 网格布局组件定义

#ifndef JZ_GRID_LAYOUT_H
#define JZ_GRID_LAYOUT_H

#include "JZLayout.h"

namespace JZ {

class GridLayout : public Layout {
public:
    GridLayout(int rows = 0, int columns = 0);
    virtual ~GridLayout();

    // 设置行列数
    void setRowCount(int rows);
    void setColumnCount(int columns);
    int rowCount() const;
    int columnCount() const;

    // 重写添加组件方法，支持网格位置
    virtual void addWidget(std::shared_ptr<Widget> widget, int stretch = 0) override;
    void addWidget(std::shared_ptr<Widget> widget, int row, int column, int rowSpan = 1, int columnSpan = 1);
    
    // 设置行高和列宽
    void setRowMinimumHeight(int row, int height);
    void setColumnMinimumWidth(int column, int width);
    
    // 设置行和列的拉伸因子
    void setRowStretch(int row, int stretch);
    void setColumnStretch(int column, int stretch);
    
    // 重写后端实现接口
    virtual void* getNativeHandle() const override;
    virtual void updateLayout() override;

protected:
    int rows_;
    int columns_;
};

} // namespace JZ

#endif // JZ_GRID_LAYOUT_H