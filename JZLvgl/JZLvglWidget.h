#ifndef JZ_LVGL_WIDGET_H_
#define JZ_LVGL_WIDGET_H_

#include "JZObject.h"
#include "JZLvglLayout.h"
#include "JZKeyEvent.h"
#include "JZPainter.h"

typedef lv_obj_t* (*lv_create_fn_t)(lv_obj_t* parent);

class JZLvglWidget : public JZObject
{
public:
    explicit JZLvglWidget(JZLvglWidget* parent = nullptr);
    virtual ~JZLvglWidget();

    lv_obj_t* handle() const { return m_lv; }

    void setParentWidget(JZLvglWidget* parent);
    JZLvglWidget* parentWidget() const;

    // 几何属性
    void setSize(int w, int h)       { lv_obj_set_size(m_lv, w, h); }
    void setPos(int x, int y)        { lv_obj_set_pos(m_lv, x, y); }
    void setSizePos(int w, int h, int x, int y) { lv_obj_set_size(m_lv, w, h); lv_obj_set_pos(m_lv, x, y); }
    void center()                    { lv_obj_center(m_lv); }
    void hide()                      { lv_obj_add_flag(m_lv, LV_OBJ_FLAG_HIDDEN); }
    void show()                      { lv_obj_remove_flag(m_lv, LV_OBJ_FLAG_HIDDEN); }
    bool isVisible() const           { return !lv_obj_has_flag(m_lv, LV_OBJ_FLAG_HIDDEN); }

    // 布局
    void setLayout(const JZLvglLayout& layout);
    JZLvglLayout layout() const      { return m_layout; }

    // 焦点
    void setFocus()                  { lv_group_focus_obj(m_lv); }
    bool hasFocus() const            { return lv_obj_has_state(m_lv, LV_STATE_FOCUSED); }

    // 重绘
    void update();

    // 预定义属性
    virtual void setProperty(const std::string& name, const JZVariant& value) override;
    virtual const JZVariant& property(const std::string& name) const override;

protected:
    // 子类用此构造函数创建 lv_obj（传入 lv_xxx_create 函数指针）
    JZLvglWidget(lv_create_fn_t create, JZLvglWidget* parent = nullptr);

    virtual void onLvEvent(lv_event_t* e);
    virtual void onKeyPressed(JZKeyEvent* event);
    virtual void paintEvent(JZPainter* painter);

    lv_obj_t* m_lv = nullptr;
    static void lvEventCallback(lv_event_t* e);

private:
    void initWidget(lv_obj_t* parentLv);

    JZLvglLayout m_layout;
};

#endif
