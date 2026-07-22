#ifndef JZ_SCROLL_AREA_H_
#define JZ_SCROLL_AREA_H_

#include "JZWidget.h"
#include "lvgl.h"

// 通用滚动区: 自身即 LVGL 可滚动对象, 通过透明占位子对象(m_content)
// 撑开滚动范围。子类在 paintEvent 中直接以视口坐标绘制可见内容, 通过
// scrollX()/scrollY()/viewportWidth()/viewportHeight() 计算绘制位置。
// 适合 owner-draw 的虚拟列表/表格。
class JZScrollArea : public JZWidget
{
    JZ_OBJECT_DECLARE(JZScrollArea)

public:
    enum ScrollBarPolicy { ScrollBarAsNeeded, ScrollBarAlwaysOff, ScrollBarAlwaysOn };

    explicit JZScrollArea(JZWidget* parent = nullptr);
    ~JZScrollArea();

    // 滚动方向(默认双向)
    void setScrollDirection(lv_dir_t dir);

    // 滚动条策略(当前 LVGL 仅支持统一模式)
    void setScrollBarPolicy(ScrollBarPolicy policy);
    void setVerticalScrollBarPolicy(ScrollBarPolicy policy);
    void setHorizontalScrollBarPolicy(ScrollBarPolicy policy);

    // 内容总大小(决定可滚动范围); 子类在内容变化时调用
    // w 或 h < 0 表示 LV_PCT(100)(跟随视口, 不产生该方向滚动)
    void setContentSize(int w, int h);
    int contentWidth() const;
    int contentHeight() const;

    // 当前滚动偏移与视口大小(绘制时使用)
    int scrollX() const;
    int scrollY() const;
    int viewportWidth() const;
    int viewportHeight() const;

    // 程序化滚动
    void scrollTo(int x, int y);
    void scrollToX(int x);
    void scrollToY(int y);
    void scrollToTop();
    void scrollToBottom();

    // 内容是否可滚动(超出视口)
    bool canScrollVertically() const;

protected:
    void onLvEvent(lv_event_t* e) override;

    // 滚动发生时回调(绘制由 LVGL 自动触发, 此处用于子类额外处理)
    virtual void onScroll();

    JZWidget* m_content = nullptr;   // 透明占位: 仅用于撑开滚动范围
};

#endif
