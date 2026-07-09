#ifndef JZ_GUI_APPLICATION_H_
#define JZ_GUI_APPLICATION_H_

#include "JZApplication.h"
#include "JZDisplayDevice.h"

class JZGUIApplication : public JZApplication
{
public:
    JZGUIApplication();
    ~JZGUIApplication();

    int screenCount() const;
    JZDisplayDevice* activeScreen() const;
    JZDisplayDevice* screen(int index) const;
    JZDisplayDevice* createScreen(int w,int h);

protected:
    friend class JZDisplayDevice;

    virtual void timerEvent(JZTimerEvent *event) override;

    void registerScreen(JZDisplayDevice *screen);
    void unregisterScreen(JZDisplayDevice *screen);

    std::vector<JZDisplayDevice*> m_screens;
    int m_refreshTimer;
};

#endif
