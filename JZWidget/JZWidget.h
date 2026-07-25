#ifndef JZ_WIDGET_WIDGET_H_
#define JZ_WIDGET_WIDGET_H_

#include "JZObject.h"
#include "JZPoint.h"
#include "JZSize.h"
#include "JZRect.h"

#include <climits>
#include <cstdint>
#include "JZLayout.h"
#include "JZKeyEvent.h"
#include "JZPainterEvent.h"
#include "JZResizeEvent.h"
#include "JZShowEvent.h"
#include "JZCloseEvent.h"

class JZWidget : public JZObject
{
    JZ_OBJECT_DECLARE(JZWidget)

public:
    explicit JZWidget(JZWidget* parent = nullptr);
    virtual ~JZWidget();

    void setParentWidget(JZWidget* parent);
    JZWidget* parentWidget() const;

    // 找到所属显示设备 (向上回溯到 JZDisplayDevice)
    class JZDisplayDevice* display() const;

    // 位置
    JZPoint pos() const;
    void move(int x, int y);
    void move(const JZPoint& p);
    int x() const;
    int y() const;

    // 尺寸
    JZSize size() const;
    void resize(int w, int h);
    void resize(const JZSize& s);
    int width() const;
    int height() const;

    JZRect rect() const;

    JZRect toGlobal(const JZRect &rect) const;

    // 尺寸约束
    void setFixedSize(int w, int h);
    void setFixedWidth(int w);
    void setFixedHeight(int h);

    void setMinimumSize(int w, int h);
    void setMinimumWidth(int w);
    void setMinimumHeight(int h);

    void setMaximumSize(int w, int h);
    void setMaximumWidth(int w);
    void setMaximumHeight(int h);

    int minimumWidth() const;
    int minimumHeight() const;
    int maximumWidth() const;
    int maximumHeight() const;

    // 尺寸策略 (参考 Qt QSizePolicy::Policy)
    enum SizePolicy {
        Fixed,            // 严格使用 sizeHint, 不增不减
        Minimum,          // 可用空间不足时保持最小, 富余时可增长
        Maximum,          // 可用空间不足时可收缩, 富余时保持 sizeHint
        Preferred,        // 可增可缩, 偏好 sizeHint
        Expanding,        // 可增可缩, 且倾向占据更多空间
        MinimumExpanding, // 倾向占据更多空间, 但不小于最小
        Ignored           // 完全忽略 sizeHint, 尽量填满可用空间
    };
    SizePolicy horizontalPolicy() const;
    SizePolicy verticalPolicy() const;
    void setSizePolicy(SizePolicy h, SizePolicy v);
    void setHorizontalPolicy(SizePolicy p);
    void setVerticalPolicy(SizePolicy p);

    // 期望尺寸 (参考 Qt QWidget::sizeHint); 设置了 baseSize 时返回 baseSize, 否则回退到当前尺寸
    JZSize sizeHint() const;

    // 几何
    JZRect geometry() const;
    void setGeometry(int x, int y, int w, int h);
    void setGeometry(const JZRect& r);    

    // 可见性
    void setVisible(bool visible);
    bool isVisible() const;

    void show();
    void hide();

    // 不透明度: 用于刷新遍历时的"剔除"判断 (等价 LVGL lv_obj 的 opaque 标志)
    void setOpaque(bool opaque);
    bool isOpaque() const;

    // 布局
    void setLayout(JZLayout* layout);
    JZLayout* layout() const;

    // 焦点
    void setFocus();
    bool hasFocus() const;

    // 重绘
    void update();
    void invalidate(const JZRect &rc);

    // 预定义属性
    virtual void setProperty(const std::string& name, const JZVariant& value) override;
    virtual JZVariant property(const std::string& name) const override;

protected:    
    friend JZDisplayDevice;

    virtual void showEvent(JZShowEvent* event);
    virtual void closeEvent(JZCloseEvent* event);
    virtual void resizeEvent(JZResizeEvent* event);

    virtual void paintEvent(JZPainterEvent* event);    

    JZLayout* m_layout = nullptr;

    int m_minW;
    int m_minH;
    int m_maxW;
    int m_maxH;

    SizePolicy m_horPolicy;
    SizePolicy m_verPolicy;

    int m_x;
    int m_y;
    int m_width;
    int m_height;

    bool m_visible;
};

#endif
