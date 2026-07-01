#include <thread>
#include <assert.h>
#include "JZApplication.h"
#include "JZStd.h"
#include "JZPlatform.h"
#include "JZDebug.h"
#include "JZTx.h"

JZApplication *jzApp = nullptr;

JZApplication::JZApplication()
    :JZObject(JZObject::MainThreadInit())
{
    JZ_ASSERT(!jzApp);
    jzApp = this;

    m_returnCode = 0;
    m_mainThread = new JZThread(JZObject::MainThreadInit());
    m_mainThread->attachStdThread(JZThread::currentThreadId());    
    m_mainThread->moveToThread(m_mainThread);
    m_mainThread->m_running = true;

    m_thread = m_mainThread;    
}

JZApplication::~JZApplication()
{
    std::vector<JZObject*> childs = m_childs;
    for (int i = 0; i < (int)childs.size(); i++)
        delete childs[i];
    m_childs.clear();

    m_thread = nullptr;

    m_mainThread->m_running = false;
    m_mainThread->detachStdThread();
    vec_removeAll(m_threads, m_mainThread);
    JZ_ASSERT(m_threads.empty());

    delete m_mainThread;
    jzApp = nullptr;
}

std::string JZApplication::applicationFilePath()
{
    return JZPlatform::applicationFilePath();
}

std::string JZApplication::applicationDirPath()
{
    return JZPlatform::applicationDirPath();
}

JZThread *JZApplication::mainThread()
{
    return m_mainThread;
}

JZThread *JZApplication::currentThread()
{
    JZMutexLocker lock(&m_mutex);
    auto id = JZThread::currentThreadId();
    for (int i = 0; i < (int)m_threads.size(); i++)
    {
        if (m_threads[i]->id() == id)
            return m_threads[i];
    }
    JZ_ASSERT(0);
    return nullptr;
}

void JZApplication::addThread(JZThread *thread)
{
    JZMutexLocker lock(&m_mutex);
    m_threads.push_back(thread);
}

void JZApplication::removeThread(JZThread *thread)
{
    JZMutexLocker lock(&m_mutex);
    vec_removeAll(m_threads, thread);
}

void JZApplication::globalLock()
{
    m_mutex.lock();
}

void JZApplication::globalUnlock()
{
    m_mutex.unlock();
}

int JZApplication::exec()
{
    JZTX_FUNC_TRACE

    m_mainThread->exec();
    return m_returnCode;
}

void JZApplication::quit()
{
    m_mainThread->quit();
}

void JZApplication::exit(int code)
{
    m_returnCode = code;
    m_mainThread->quit();
}

void JZApplication::processEvents()
{
    JZTX_FUNC_TRACE
    currentThread()->processEvents();
}

void JZApplication::sendEvent(JZObject *object, JZEvent *event)
{
    if (object->thread() == currentThread())
        object->event(event);
    else
    {
        postEventAndWait(object, event);        
    }
}

void JZApplication::postEvent(JZObject *object, JZEvent *event)
{
    JZEventInfo e(object, event);
    object->thread()->postEvent(e);
}

void JZApplication::postEventAndWait(JZObject *object, JZEvent *event)
{
    JZSemaphore sem;
    
    JZEventInfo e(object, event);
    e.sem = &sem;
    object->thread()->postEvent(e);

    sem.acquire();
}