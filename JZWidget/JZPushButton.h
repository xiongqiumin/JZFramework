#ifndef JZ_WIDGET_BUTTON_H_
#define JZ_WIDGET_BUTTON_H_

#include "JZWidget.h"
#include "JZSignal.h"

class JZPushButton : public JZWidget
{
    JZ_OBJECT_DECLARE(JZPushButton)

public:
    explicit JZPushButton(JZWidget* parent = nullptr);
    JZPushButton(JZWidget* parent, int w, int h, const std::string& text);

    lv_obj_t* labelHandle() const;

    void setText(const std::string& text);
    std::string text() const;

    void setWidth(int w);
    void setHeight(int h);

jz_signals:
    JZSigVoid sigClicked;

protected:
    void onLvEvent(lv_event_t* e) override;

private:
    lv_obj_t* m_label = nullptr;
};

#endif
