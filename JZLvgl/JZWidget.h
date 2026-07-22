#ifndef JZ_LVGL_WIDGET_H_
#define JZ_LVGL_WIDGET_H_

#include "JZObject.h"
#include "JZPoint.h"
#include "JZSize.h"
#include "JZRect.h"
#include "JZLayout.h"
#include "JZKeyEvent.h"
#include "JZPainterEvent.h"
#include "JZResizeEvent.h"
#include "JZShowEvent.h"
#include "JZCloseEvent.h"

typedef lv_obj_t* (*lv_create_fn_t)(lv_obj_t* parent);

class JZWidget : public JZObject
{
    JZ_OBJECT_DECLARE(JZWidget)

public:
    explicit JZWidget(JZWidget* parent);
    virtual ~JZWidget();

    lv_obj_t* handle() const;

    void setParentWidget(JZWidget* parent);
    JZWidget* parentWidget() const;

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

    // 几何
    JZRect geometry() const;
    void setGeometry(int x, int y, int w, int h);
    void setGeometry(const JZRect& r);

    // 可见性
    void setVisible(bool visible);
    bool isVisible() const;

    void show();
    void hide();

    // 布局
    void setLayout(JZLayout* layout);
    JZLayout* layout() const;

    // 焦点
    void setFocus();
    bool hasFocus() const;

    // 重绘
    void update();

    // 预定义属性
    virtual void setProperty(const std::string& name, const JZVariant& value) override;
    virtual JZVariant property(const std::string& name) const override;

    // 绘制层（paintEvent 中有效）
    lv_layer_t* paintLayer() const { return m_paintLayer; }

protected:
    struct NoCreate{};

    static void lvEventCallback(lv_event_t* e);
        
    // 子类用此构造函数创建 lv_obj（传入 lv_xxx_create 函数指针）
    JZWidget(lv_create_fn_t create, JZWidget* parent);
    JZWidget(NoCreate, JZWidget* parent = nullptr);
    
    void setLvglObject(lv_obj_t* obj);

    virtual void onLvEvent(lv_event_t* e);
    virtual void onKeyPressed(JZKeyEvent* event);
    virtual void paintEvent(JZPainterEvent* event);

    virtual void showEvent(JZShowEvent* event);
    virtual void closeEvent(JZCloseEvent* event);
    virtual void resizeEvent(JZResizeEvent* event);

    JZLayout* m_layout = nullptr;
    lv_obj_t* m_lv = nullptr;
    lv_layer_t* m_paintLayer = nullptr;
};

#endif
