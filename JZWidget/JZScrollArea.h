#ifndef JZ_SCROLL_AREA_H_
#define JZ_SCROLL_AREA_H_
#include "JZWidget.h"

class JZScrollBar;

// 通用滚动区 (Qt QScrollArea 语义重构版)。
//
// 内置真实的 viewport 视口与 JZScrollBar 滚动条:
//   - 滚动条: 垂直/水平两条真实 JZScrollBar 子控件, 按策略 (ScrollBarAsNeeded /
//     AlwaysOff / AlwaysOn) 自动显隐, 范围/页码/值随内容与视口实时同步, 可拖拽滑块;
//   - 内容模型 A —— setWidget(真实内容控件): 内容控件挂到内部 viewport 子控件上,
//     滚动 = 把内容控件平移到 viewport 内的 (-scrollX, -scrollY); viewport 开启
//     setClipChildren(), 越出视口的内容在绘制阶段被裁掉, 不再画到滚动区/滚动条上;
//   - 内容模型 B —— owner-draw 虚拟内容: 子类 (List/Table/Tree, 经 JZAbstractViewWidget)
//     在自己的 paintEvent 里按 viewportWidth()/viewportHeight()/scrollX()/scrollY()
//     直接绘制可见内容, 并通过 setContentSize() 上报内容总尺寸驱动滚动条。
//
// 滚轮: 未消费的滚轮事件会沿控件父链冒泡 (JZWindow 实现), 因此在内容控件及其任意
// 子控件上滚动也能让本滚动区滚动。
class JZFRAMEWORK_API JZScrollArea : public JZWidget
{
    JZ_OBJECT_DECLARE(JZScrollArea)

public:
    enum ScrollBarPolicy {
        ScrollBarAsNeeded,   // 内容超出视口才显示
        ScrollBarAlwaysOff,
        ScrollBarAlwaysOn
    };

    explicit JZScrollArea(JZWidget* parent = nullptr);
    ~JZScrollArea();

    // ---- 滚动方向 (兼容保留; 内容尺寸为 -1 的方向本身不会滚动) ----
    void setScrollDirection(int dir);

    void setScrollBarPolicy(ScrollBarPolicy policy);
    void setVerticalScrollBarPolicy(ScrollBarPolicy policy);
    void setHorizontalScrollBarPolicy(ScrollBarPolicy policy);
    ScrollBarPolicy verticalScrollBarPolicy() const;
    ScrollBarPolicy horizontalScrollBarPolicy() const;

    // ---- 真实滚动条 (始终存在, 是否可见由策略决定) ----
    JZScrollBar* verticalScrollBar() const;
    JZScrollBar* horizontalScrollBar() const;

    // ---- 内容控件 (Qt QScrollArea::setWidget) ----
    // 内容控件成为内部 viewport 的子控件, 由滚动区接管其生命周期
    // (再次 setWidget 或滚动区析构时会被删除)。内容总尺寸取自内容控件的
    // width()/height(), 请自行 resize (未定尺寸时按 sizeHint() 初始化)。
    void setWidget(JZWidget* content);
    JZWidget* widget() const;

    // ---- 内容自适应 (Qt QScrollArea::setWidgetResizable 语义) ----
    // 开启后由滚动区负责给内容控件定尺寸: 每个轴取 max(内容的 minimumSizeHint,
    // 内容的 sizeHint, 视口尺寸)。内容"最小尺寸"比视口大时保持内容尺寸, 于是出现
    // 滚动条 —— 这正是"固定尺寸卡片 + 网格"想要的(bteserver 原版 QScrollArea 就是
    // widgetResizable=true)。关闭(默认)时沿用旧契约: 调用方自己 resize 内容控件。
    void setWidgetResizable(bool on);
    bool widgetResizable() const { return m_widgetResizable; }
    // 内部视口子控件 (内容控件的父), 调用 setWidget 前为 nullptr
    JZWidget* viewport() const;

    // ---- 内容对齐 (Qt QScrollArea::setAlignment 语义) ----
    // **内容比视口小**时, 内容在视口里的摆放位置 (居中 = AlignHCenter|AlignVCenter);
    // 内容溢出时这一项不起作用 —— 那时位置由滚动偏移决定 (永远贴左上, 也就是默认值)。
    // 取值按 Qt 的位标志: 水平取 AlignLeft/AlignRight/AlignHCenter 之一, 垂直取
    // AlignTop/AlignBottom/AlignVCenter 之一 (同时给多个时优先级与 Qt 一致:
    // 居中最优先, 其次右/下; 默认 AlignLeft|AlignTop)。
    enum AlignmentFlag {
        AlignLeft    = 1,
        AlignRight   = 2,
        AlignHCenter = 4,
        AlignTop     = 8,
        AlignBottom  = 16,
        AlignVCenter = 32
    };
    void setAlignment(int flags);            // 内容位置随之更新
    int alignment() const { return m_alignment; }

    // ---- 内容总大小 (模型 B: 虚拟内容范围) ----
    // w/h < 0 表示该方向跟随视口 (不产生滚动条)。
    void setContentSize(int w, int h);
    int contentWidth() const;
    int contentHeight() const;

    // ---- 滚动状态 ----
    int scrollX() const;
    int scrollY() const;
    int maxScrollX() const;   // 0 = 不可水平滚动
    int maxScrollY() const;
    // 视口尺寸 = 自身尺寸扣除当前可见的滚动条带宽 (模型 B 的内容绘制范围)
    int viewportWidth() const;
    int viewportHeight() const;

    void scrollTo(int x, int y);
    void scrollToX(int x);
    void scrollToY(int y);
    void scrollToTop();
    void scrollToBottom();

    bool canScrollVertically() const;
    bool canScrollHorizontally() const;

    JZSize sizeHint() const override;

    void paintEvent(JZPainterEvent* event) override;
    void wheelEvent(JZMouseEvent* event) override;
    void keyPressEvent(JZKeyEvent* event) override;

protected:
    // 事件过滤: 拦截内部 viewport 的 paint 事件, 绘制视口背景 (Qt 语义)
    bool eventFilter(JZObject* watched, JZEvent* event) override;
    void resizeEvent(JZResizeEvent* event) override;

    // 重新摆放/同步滚动条与 viewport (内容或视口尺寸变化后调用)
    void layoutScrollBars();

    int m_contentW = -1;   // 模型 B 内容总尺寸 (-1 = 跟随视口)
    int m_contentH = -1;
    int m_scrollX = 0;
    int m_scrollY = 0;
    int m_alignment = AlignLeft | AlignTop;   // 内容比视口小时的位置 (见 setAlignment)

    ScrollBarPolicy m_vPolicy = ScrollBarAsNeeded;
    ScrollBarPolicy m_hPolicy = ScrollBarAsNeeded;

    JZScrollBar* m_vbar = nullptr;
    JZScrollBar* m_hbar = nullptr;
    JZWidget* m_viewport = nullptr;       // setWidget 模式: 内部视口 (懒创建)
    JZWidget* m_widgetContent = nullptr;  // setWidget 模式: 内容控件
    bool m_widgetResizable = false;       // 内容控件由滚动区定尺寸 (Qt widgetResizable)
    bool m_inResizeContent = false;       // 给内容 resize 时防重入 (内容布局可能再触发本函数)
};

#endif // JZ_SCROLL_AREA_H_
