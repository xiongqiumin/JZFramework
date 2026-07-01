#ifndef JZ_LVGL_BUTTON_H_
#define JZ_LVGL_BUTTON_H_

#include "JZLvglWidget.h"
#include "JZSignal.h"

class JZLvglButton : public JZLvglWidget
{
public:
    JZSigVoid sigClicked;

    explicit JZLvglButton(JZLvglWidget* parent = nullptr);
    JZLvglButton(JZLvglWidget* parent, int w, int h, const std::string& text);

    lv_obj_t* labelHandle() const { return m_label; }

    void setText(const std::string& text);
    std::string text() const;

    void setWidth(int w)  { setSize(w, lv_obj_get_height(m_lv)); }
    void setHeight(int h) { setSize(lv_obj_get_width(m_lv), h); }

protected:
    void onLvEvent(lv_event_t* e) override;

private:
    lv_obj_t* m_label = nullptr;
};

#endif
