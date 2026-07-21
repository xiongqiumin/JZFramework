#include "JZEventLoop.h"
#include "JZThread.h"

//JZEventLoop
JZEventLoop::JZEventLoop()
{
    m_running = false;
    m_code = 0;
}

JZEventLoop::~JZEventLoop()
{
}

int JZEventLoop::exec()
{
    JZTX_FUNC_TRACE
    m_running = true;
    while (m_running)
    {
        processEvents();
        thread()->waitEvents();
    }    
    return m_code;
}

void JZEventLoop::quit()
{
    exit(0);
}

void JZEventLoop::exit(int code)
{
    m_code = code; 
    m_running = false;
    thread()->wakeUp();   // 立即唤醒 waitEvents, 让 exec() 循环尽快退出
}

void JZEventLoop::processEvents()
{
    JZTX_FUNC_TRACE
    thread()->processEvents();
}