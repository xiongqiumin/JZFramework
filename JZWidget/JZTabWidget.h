#ifndef JZ_TAB_WIDGET_H_
#define JZ_TAB_WIDGET_H_

#include "JZWidget.h"
#include "JZSignal.h"
#include <vector>
#include <string>

// 类似 Qt QTabWidget: 顶部标签条 (owner-draw) + 下方页面区。
//
// 内部不再派生 JZStackedWidget, 而是持有一个普通 JZWidget 子控件作为页面容器
// (m_pageArea): 所有页面都是 m_pageArea 的直接子控件, 由本控件切换显隐; 页面区
// 背景通过 installEventFilter + eventFilter 拦截 m_pageArea 的 paint 事件来绘制。
//
//     JZTabWidget* tabs = new JZTabWidget(parent);
//     tabs->addTab(new JZWidget(), "General");
//     tabs->addTab(new JZWidget(), "Advanced");
//     connect(&tabs->currentChanged, ...);
class JZTabWidget : public JZWidget
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

    void resize(int w, int h) override;
    void setGeometry(int x, int y, int w, int h) override;
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
    int tabWidth(int index) const;        // 标签宽度 = 文本宽 + padding
    int tabAt(int x) const;               // 本控件坐标 x -> 标签索引, 未命中 -1
    void relayout();                      // 页面容器铺满标签条以下并重排页面
    void showCurrentPage();               // 仅当前页可见 (其余隐藏)

    JZWidget* m_pageArea = nullptr;       // 内部页面容器 (普通 JZWidget)
    std::vector<JZWidget*> m_pages;       // 与 m_labels 一一对应
    std::vector<std::string> m_labels;
    int m_current = -1;

    static const int kTabBarH = 28;       // 标签条高度
};

#endif // JZ_TAB_WIDGET_H_
