#include <assert.h>
#include "JZSignal.h"
#include "JZApplication.h"

//JZAsyncSignalEvent
JZAsyncSignalEvent::JZAsyncSignalEvent()
    :JZEvent(JZEvent_asyncSignal)
{
    hasRecv = false;
    callback = nullptr;
}

JZAsyncSignalEvent::~JZAsyncSignalEvent()
{
    if(callback)
        delete callback;
}

//JZBaseSignal
JZBaseSignal::JZBaseSignal()
{
}

JZBaseSignal::~JZBaseSignal()
{
}

JZThread *JZBaseSignal::objectThread(JZObject*sender)
{
    return sender->thread();
}

void JZBaseSignal::pushSender(JZThread* thread, JZObject *sender)
{
    //sigDestroy 时 thread 已经删除
    if(thread)
        thread->pushSender(sender);
}

void JZBaseSignal::popSender(JZThread* thread)
{
    if (thread)
        thread->popSender();
}

JZObject* JZBaseSignal::currentThread()
{
    return jzApp->currentThread();
}

bool JZBaseSignal::isCurrentThread(JZObject *recv)
{
    return recv->thread()->id() == JZThread::currentThreadId();
}

bool JZBaseSignal::isBlockSingals(JZObject *object)
{
    return object->isBlockSignals();
}

JZObjectPointer JZBaseSignal::guardPointer(JZObject *obj)
{
    return obj->guardPointer();
}

void JZBaseSignal::postEvent(JZAsyncSignalEvent* event)
{
    JZThread* t = (JZThread*)currentThread();

    JZEventInfo e(t, event);
    t->postEvent(e);
}

void JZBaseSignal::postEvent(JZObject *recvier, JZAsyncSignalEvent*event)
{
    jzApp->postEvent(recvier, event);
}

void JZBaseSignal::postEventAndWait(JZObject *recvier, JZAsyncSignalEvent *event)
{
    jzApp->sendEvent(recvier, event);
    delete event;
}