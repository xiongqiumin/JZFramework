#ifndef JZ_LVGL_WIDGET_H_
#define JZ_LVGL_WIDGET_H_

#include "JZObject.h"
#include "JZLayout.h"
#include "JZKeyEvent.h"
#include "JZPainter.h"

typedef lv_obj_t* (*lv_create_fn_t)(lv_obj_t* parent);

class JZWidget : public JZObject
{
    JZ_OBJECT_DECLARE(JZWidget)

public:
    explicit JZWidget(JZWidget* parent = nullptr);
    virtual ~JZWidget();

    lv_obj_t* handle() const;

    void setParentWidget(JZWidget* parent);
    JZWidget* parentWidget() const;

    // 几何属性
    void setSize(int w, int h);
    void setPos(int x, int y);
    void setSizePos(int w, int h, int x, int y);
    void center();
    void hide();
    void show();
    bool isVisible() const;

    // 布局
    void setLayout(const JZLayout& layout);
    JZLayout layout() const;

    // 焦点
    void setFocus();
    bool hasFocus() const;

    // 重绘
    void update();

    // 预定义属性
    virtual void setProperty(const std::string& name, const JZVariant& value) override;
    virtual const JZVariant& property(const std::string& name) const override;

protected:
    struct NoCreate{};

    static void lvEventCallback(lv_event_t* e);
        
    // 子类用此构造函数创建 lv_obj（传入 lv_xxx_create 函数指针）
    JZWidget(lv_create_fn_t create, JZWidget* parent = nullptr);
    JZWidget(NoCreate, JZWidget* parent = nullptr);
    
    void setLvglObject(lv_obj_t* obj);

    virtual void onLvEvent(lv_event_t* e);
    virtual void onKeyPressed(JZKeyEvent* event);
    virtual void paintEvent(JZPainter* painter);    

    JZLayout m_layout;
    lv_obj_t* m_lv = nullptr;
};

#endif
