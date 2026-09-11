#include "JZEventLoop.h"
#include "JZThread.h"
#include "JZApplication.h"
#include "JZDebug.h"

#ifdef _WIN32
#include "JZWin32Thread.h"
#endif

//JZEventLoop
JZEventLoop::JZEventLoop()
{
    m_running = false;
    m_code = 0;
}

JZEventLoop::~JZEventLoop()
{
}

bool JZEventLoop::isRunning() const
{
    return m_running;
}

int JZEventLoop::code()
{
    return m_code;
}

int JZEventLoop::exec()
{
    JZTX_FUNC_TRACE

    // event_loop 所在 thread 可能和当前thread不一致.
    auto cur_thread = jzApp->currentThread();
    m_running = true;
#ifdef _WIN32    
    if (thread() == jzApp->mainThread())
    {
        return ((JZWin32Thread*)cur_thread)->execLoop(this);
    }
#endif

    while (m_running)
    {
        cur_thread->processEvents();
        cur_thread->waitEvents();
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
    jzApp->processEvents();
}