#include "JZGuiApplication.h"

JZGUIApplication::JZGUIApplication()
    : JZApplication()
    , m_refreshTimer(0)
{
    lv_init();
    auto device = new JZDisplayDevice(800, 600);
    registerScreen(device);

    m_refreshTimer = startTimer(33);
}

JZGUIApplication::~JZGUIApplication()
{
    killTimer(m_refreshTimer);
}

void JZGUIApplication::timerEvent(JZTimerEvent *event)
{
    lv_timer_handler();
}

JZDisplayDevice* JZGUIApplication::createScreen(int w, int h)
{
    auto* screen = new JZDisplayDevice(800, 600);
    registerScreen(screen);
    return screen;
}

int JZGUIApplication::screenCount() const
{
    return (int)m_screens.size();
}

JZDisplayDevice* JZGUIApplication::activeScreen() const
{
    return nullptr;
}

JZDisplayDevice* JZGUIApplication::screen(int index) const
{
    if (index < 0 || index >= (int)m_screens.size())
        return nullptr;
    return m_screens[index];
}

void JZGUIApplication::registerScreen(JZDisplayDevice *screen)
{
    if (!screen) 
        return;

    for (auto* s : m_screens) {
        if (s == screen) return;
    }
    m_screens.push_back(screen);
}

void JZGUIApplication::unregisterScreen(JZDisplayDevice *screen)
{
    for (auto it = m_screens.begin(); it != m_screens.end(); ++it) {
        if (*it == screen) {
            m_screens.erase(it);
            return;
        }
    }
}
