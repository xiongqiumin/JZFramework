#include "JZEventLoop.h"
#include "JZThread.h"

//JZEventLoop
JZEventLoop::JZEventLoop()
{
    m_running = false;
}

JZEventLoop::~JZEventLoop()
{
}

void JZEventLoop::exec()
{
    m_running = true;
    while (m_running)
    {
        processEvents();
        thread()->waitEvents();
    }    
}

void JZEventLoop::quit()
{
    exit();
}

void JZEventLoop::exit()
{
    m_running = false;        
}

void JZEventLoop::processEvents()
{
    thread()->processEvents();
}