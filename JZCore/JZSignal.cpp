#include <assert.h>
#include "JZSignal.h"
#include "JZApplication.h"

//JZAsyncSignalEvent
JZAsyncSignalEvent::JZAsyncSignalEvent()
    :JZEvent(JZEvent_asyncSignal)
{
    hasRecv = false;
}

JZAsyncSignalEvent::~JZAsyncSignalEvent()
{
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
    thread->pushSender(sender);
}

void JZBaseSignal::popSender(JZThread* thread)
{
    thread->popSender();
}

JZObject* JZBaseSignal::currentThread()
{
    return jzApp->currentThread();
}

bool JZBaseSignal::isCurrentThread(JZObject *recv)
{
    if (!recv)
        return true;

    return recv->thread() == jzApp->currentThread();
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
    jzApp->postEventAndWait(recvier, event);
}