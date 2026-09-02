#ifndef JZ_TAB_WIDGET_H_
#define JZ_TAB_WIDGET_H_

#include "JZWidget.h"
#include "JZSignal.h"
#include "JZStackedWidget.h"
#include <vector>
#include <string>

// 类似 Qt QTabWidget: 顶部标签条 (owner-draw) + 下方页面区 (内嵌 JZStackedWidget)。
//   - addTab 把页面挂到内部 stacked, 页面随本控件尺寸铺满标签条以下区域;
//   - 点击标签切换页面并发 currentChanged;
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

    // 页面所有权: 加入后由本控件 (经内部 stacked) 持有
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

private:
    int tabWidth(int index) const;        // 标签宽度 = 文本宽 + padding
    int tabAt(int x) const;               // 本控件坐标 x -> 标签索引, 未命中 -1
    void relayout();                      // 页面区铺满标签条以下

    JZStackedWidget* m_stack = nullptr;
    std::vector<std::string> m_labels;    // 与 stacked 页面一一对应
    int m_current = -1;

    static const int kTabBarH = 28;       // 标签条高度
};

#endif // JZ_TAB_WIDGET_H_
