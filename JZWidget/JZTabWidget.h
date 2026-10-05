#ifndef JZ_TAB_WIDGET_H_
#define JZ_TAB_WIDGET_H_
#include "JZWidget.h"
#include "JZSignal.h"
#include "JZRect.h"
#include <vector>
#include <string>

// 类似 Qt QTabWidget: 标签条 (owner-draw) + 页面区。
//
// 内部不再派生 JZStackedWidget, 而是持有一个普通 JZWidget 子控件作为页面容器
// (m_pageArea): 所有页面都是 m_pageArea 的直接子控件, 由本控件切换显隐; 页面区
// 背景通过 installEventFilter + eventFilter 拦截 m_pageArea 的 paint 事件来绘制。
//
//     JZTabWidget* tabs = new JZTabWidget(parent);
//     tabs->addTab(new JZWidget(), "General");
//     tabs->addTab(new JZWidget(), "Advanced");
//     tabs->setTabPosition(JZTabWidget::West);   // 标签条放到左边(Qt: setTabPosition)
//     connect(&tabs->currentChanged, ...);
//
// 标签条方向(对齐 Qt QTabWidget::TabPosition):
//   North(默认) 上 / South 下 / West 左 / East 右。
//   North/South 时标签沿水平排列、条厚 = 高度; West/East 时沿垂直排列、条厚 = 宽度,
//   标签文字用画布矩阵旋转 90°(见 .cpp 的 paintTabText)。
class JZFRAMEWORK_API JZTabWidget : public JZWidget
{
    JZ_OBJECT_DECLARE(JZTabWidget)

public:
    explicit JZTabWidget(JZWidget* parent = nullptr);

    // 页面所有权: 加入后由本控件持有
    int addTab(JZWidget* page, const std::string& label);
    void removeTab(int index);            // 同时删除页面

    JZWidget* widget(int index) const;
    int indexOf(JZWidget* page) const;
    int count() const;
    std::string tabText(int index) const;

    int currentIndex() const;
    void setCurrentIndex(int index);      // 切换并发 currentChanged
    // 当前页 / 切到指定页(等价 QTabWidget::currentWidget / setCurrentWidget):
    // page 不是本控件的页时什么都不做(Qt 同款)。
    JZWidget* currentWidget() const;
    void setCurrentWidget(JZWidget* page);

    // 标签条方向(对齐 Qt QTabWidget::TabPosition; 取值顺序也与 Qt 一致)
    enum TabPosition
    {
        North = 0,   // 上(默认)
        South,       // 下
        West,        // 左
        East,        // 右
    };
    void setTabPosition(TabPosition pos);
    TabPosition tabPosition() const { return m_tabPosition; }

    // 尺寸变化统一走 resizeEvent(框架钩子): 页面区要按标签条方向让位, 只有它自己算得出。
    // 不必 override setGeometry/resize —— 二者最终都会派发本事件。
    void resizeEvent(JZResizeEvent* event) override;
    JZSize sizeHint() const override;

jz_signals:
    JZSigInt currentChanged;

public:
    // ---- AI 语义 ----
    std::string aiText() const override { return tabText(m_current); }
    std::vector<std::string> aiItems() const override { return m_labels; }
    std::vector<JZAiAction> aiActions() const override { return {{"select", "number 页索引"}}; }

protected:
    void paintEvent(JZPainterEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;
    bool eventFilter(JZObject* watched, JZEvent* event) override;

private:
    // ---- 几何(全部随 m_tabPosition 变) ----
    int   tabLength(int index) const;         // 单个标签**沿标签条方向**的长度(文本宽 + padding)
    int   tabThickness() const;               // 标签条厚度(North/South = 高度, West/East = 宽度)
    JZRect tabBarRect() const;                // 标签条整体矩形
    JZRect tabRect(int index) const;          // 第 index 个标签的矩形
    JZRect contentRect() const;               // 页面区矩形(不含标签条那一侧)
    int   tabAt(int pos) const;               // 沿标签条方向的坐标 -> 标签索引, 未命中 -1
    bool  inTabBar(const JZPoint& pt) const;  // 点是否落在标签条上
    void  paintTabText(JZPainter& p, const JZRect& r, const std::string& text, bool sel);
    void  relayout();                         // 页面容器铺满页面区并重排页面
    void  showCurrentPage();                  // 仅当前页可见 (其余隐藏)

    JZWidget* m_pageArea = nullptr;       // 内部页面容器 (普通 JZWidget)
    std::vector<JZWidget*> m_pages;       // 与 m_labels 一一对应
    std::vector<std::string> m_labels;
    int m_current = -1;

    TabPosition m_tabPosition = North;
    static const int kTabBarThickness = 28;   // 标签条厚度(原kTabBarH: North/South 时是高度)
    static const int kTabMinLength = 48;      // 单个标签的最小长度
};

#endif // JZ_TAB_WIDGET_H_
