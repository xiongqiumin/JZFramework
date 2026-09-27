#ifndef JZ_WIDGET_SPLITTER_H_
#define JZ_WIDGET_SPLITTER_H_
#include "JZWidget.h"
#include "JZSignal.h"
#include <vector>

// 类似 Qt QSplitter 的可拖拽分隔容器: 沿一个方向排布若干子控件,
// 控件之间用可拖动的分隔条 (handle) 调整各自占比。本控件自行管理子控件几何
// (不依赖 m_layout), 通过重写 resize() 在容器尺寸变化时重排子控件。
class JZFRAMEWORK_API JZSplitter : public JZWidget
{
    JZ_OBJECT_DECLARE(JZSplitter)

public:
    enum Orientation {
        Horizontal,   // 子控件左右排布, 分隔条为竖条
        Vertical      // 子控件上下排布, 分隔条为横条
    };

    explicit JZSplitter(Orientation o = Horizontal, JZWidget* parent = nullptr);
    ~JZSplitter() override;

    // 方向
    void setOrientation(Orientation o);
    Orientation orientation() const;

    // 子控件管理 (等价 QSplitter::addWidget / insertWidget / widget / indexOf)
    void addWidget(JZWidget* widget);
    void insertWidget(int index, JZWidget* widget);
    void removeWidget(JZWidget* widget);
    int count() const;
    JZWidget* widget(int index) const;
    int indexOf(JZWidget* widget) const;

    // 拉伸因子 (初次分配/容器缩放时按此比例分配空间; 等价 QSplitter::setStretchFactor)
    void setStretchFactor(int index, int stretch);
    int stretchFactor(int index) const;

    // 分隔条宽度 (像素)
    void setHandleWidth(int w);
    int handleWidth() const;

    // 是否允许被拖到 0 尺寸 (不可折叠且控件最小尺寸为 0 时, 给一个 20px 下限)
    void setCollapsible(int index, bool collapsible);
    bool isCollapsible(int index) const;

    // 直接设定/读取各子控件沿排布方向的像素尺寸 (等价 QSplitter::setSizes / sizes)
    // 隐藏的控件尺寸恒为 0；重新显示后它的尺寸需要由调用方重新设定（本控件只保证有最小尺寸）。
    void setSizes(const std::vector<int>& sizes);
    std::vector<int> sizes() const;

    JZSize sizeHint() const override;

jz_signals:
    JZSignal<int, int> sigSplitterMoved;   // (pos, handleIndex) 拖动结束后/过程中发出

protected:
    void resize(int w, int h) override;
    void paintEvent(JZPainterEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;
    void mouseMoveEvent(JZMouseEvent* event) override;
    void mouseReleaseEvent(JZMouseEvent* event) override;
    bool eventFilter(JZObject* watched, JZEvent* event) override;
    void childEvent(JZChildEvent* event) override;

    // 按当前几何 + m_sizes 摆放子控件; m_sizes 未初始化时按拉伸因子分配。
    // 隐藏(setVisible(false))的子控件不参与: 尺寸为 0、不占空间、也不产生分隔条，
    // 剩下的可见控件平分整个区域（等价 Qt 里"隐藏项被布局剔除"的语义）。
    void relayout();
    int totalStretch() const;
    int minOf(int index) const;                      // 子控件沿排布方向的最小尺寸
    int handleIndexAt(const JZPoint& p) const;       // 点 p 落在哪条分隔条上 (-1 表示无)

    // ---- 可见性 ----
    bool isItemVisible(int index) const;             // 子控件存在且 isVisible()
    int  visibleCount() const;                       // 可见子控件个数
    // 重算分隔条与子控件的对应关系: 第 h 条分隔条夹在 m_handleLeft[h] 与 m_handleRight[h] 之间。
    // 隐藏项被跳过，所以两者不一定是相邻下标。
    void rebuildHandles(const std::vector<int>& visible);

    // 悬停/拖动光标: 指针在分隔条 (或拖动中) 上显示双向调整箭头, 否则清除。
    // 参数为屏幕逻辑坐标 (子面板上移动时由 eventFilter 转发过来)。
    void updateHoverCursor(const JZPoint& global);

    // 事件过滤只装在本控件的直接子面板上, 但分隔条热区下方可能还嵌套着更深的
    // 子孙控件(如竖向 splitter 里"工具条 + 输入框"又套了一层容器): 指针移到这些
    // 孙子控件上时, 直接子面板的过滤器收不到事件, 导致离开分隔条后悬停光标(双向
    // 箭头)无法清除、卡在箭头状态。因此过滤要递归装到所有子孙, 动态新增的也靠
    // childAdded 补装, 移除时靠 childRemoved 摘除(避免悬垂过滤器)。
    void installChildFilter(JZObject* obj);
    void removeChildFilter(JZObject* obj);

    struct Item {
        JZWidget* widget = nullptr;
        int stretch = 1;
        bool collapsible = true;
    };

    Orientation m_orientation = Horizontal;
    int m_handleWidth = 6;
    std::vector<Item> m_items;       // 每个被管理的子控件一项
    std::vector<int> m_sizes;        // 各子控件沿排布方向像素尺寸, 长度 == m_items.size()
    // 分隔条 -> 左右子控件下标（按当前可见性在 relayout 里重建; 隐藏项不产生分隔条）
    std::vector<int> m_handleLeft;
    std::vector<int> m_handleRight;

    int m_dragHandle = -1;           // 正在拖动的分隔条序号 (-1 无)
    int m_dragStart = 0;             // 拖动起点 (沿排布方向坐标)
    std::vector<int> m_dragSizes;    // 拖动开始时的 m_sizes 快照
    int m_hoverHandle = -1;          // 悬停高亮的分隔条
};

#endif // JZ_WIDGET_SPLITTER_H_
