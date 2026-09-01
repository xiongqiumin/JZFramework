#ifndef JZ_WIDGET_WIDGET_H_
#define JZ_WIDGET_WIDGET_H_

#include "JZObject.h"
#include "JZPoint.h"
#include "JZSize.h"
#include "JZRect.h"
#include "JZStyleSet.h"

#include <climits>
#include <cstdint>
#include "JZLayout.h"
#include "JZKeyEvent.h"
#include "JZMouseEvent.h"
#include "JZPainterEvent.h"
#include "JZResizeEvent.h"
#include "JZShowEvent.h"
#include "JZCloseEvent.h"
#include "JZHideEvent.h"

class JZWidget : public JZObject
{
    JZ_OBJECT_DECLARE(JZWidget)

public:
    explicit JZWidget(JZWidget* parent = nullptr);
    virtual ~JZWidget();

    void setParentWidget(JZWidget* parent);
    JZWidget* parentWidget() const;

    // 找到所属显示设备 (回溯到根控件, 由其提供渲染设备)
    class JZWindow* window();

    // 位置
    JZPoint pos() const;
    void move(int x, int y);
    void move(const JZPoint& p);
    int x() const;
    int y() const;

    // 尺寸
    JZSize size() const;
    virtual void resize(int w, int h);   // 虚: 显示设备重写以同步原生窗口尺寸
    void resize(const JZSize& s);
    int width() const;
    int height() const;

    JZRect rect() const;

    JZPoint mapTo(JZWidget* parent, const JZPoint& rect) const;
    JZPoint mapToGlobal(const JZPoint& rect) const;

    JZRect mapTo(JZWidget *parent, const JZRect& rect) const;
    JZRect mapToGlobal(const JZRect &rect) const;

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

    // 期望尺寸 (参考 Qt QWidget::sizeHint); 默认回退到当前尺寸, 容器/控件可重写
    virtual JZSize sizeHint() const;

    // 几何
    JZRect geometry() const;
    virtual void setGeometry(int x, int y, int w, int h);
    virtual void setGeometry(const JZRect& r);

    // 可见性
    void setVisible(bool visible);
    bool isVisible() const;

    void show();
    void hide();

    // 不透明度: 用于刷新遍历时的"剔除"判断 (等价 LVGL lv_obj 的 opaque 标志)
    void setOpaque(int opaque);
    int opaque() const;

    // 布局
    void setLayout(JZLayout* layout);
    JZLayout* layout() const;

    // 控件样式 (背景/边框/圆角/padding)。paintEvent 内经 JZStylePainter 主动绘制;
    // 后续由样式表 (JZStyleSheet) 解析填充。
    JZStyleSet& style() { return m_style; }
    const JZStyleSet& style() const { return m_style; }

    // 重排:
    //   updateLayout()   只重排自身子控件(布局应用 / resize 内部使用, 不向上传播);
    //   updateGeometry() 在本控件尺寸特征(策略/约束/布局内容)变化后, 向上级联重排
    //                    (父布局持有本控件作为 item, 必须重排; 再继续向上直到根)。
    void updateLayout();
    void updateGeometry();

    // 焦点
    void setFocus();
    bool hasFocus() const;

    // 重绘
    void update();
    void invalidate(const JZRect &rc);

    // 事件投递 (显示设备向控件树分发鼠标/键盘事件用)
    bool sendEvent(JZEvent* event);

    // 预定义属性
    virtual void setProperty(const std::string& name, const JZVariant& value) override;
    virtual JZVariant property(const std::string& name) const override;

protected:
    virtual bool event(JZEvent* event) override;
    virtual void showEvent(JZShowEvent* event);
    virtual void hideEvent(JZHideEvent* event);
    virtual void closeEvent(JZCloseEvent* event);
    virtual void resizeEvent(JZResizeEvent* event);
    virtual void paintEvent(JZPainterEvent* event);

    // 鼠标事件 (坐标为逻辑像素, localPos 相对本控件)。命中检测由
    // JZDisplayDevice 完成, ImGui 等渲染后端不参与交互。
    virtual void mousePressEvent(JZMouseEvent* event);
    virtual void mouseReleaseEvent(JZMouseEvent* event);
    virtual void mouseMoveEvent(JZMouseEvent* event);
    virtual void wheelEvent(JZMouseEvent* event);

    JZLayout* m_layout = nullptr;

    JZStyleSet m_style;

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
