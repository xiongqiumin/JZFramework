#include <assert.h>
#include "JZSignal.h"
#include "JZApplication.h"

//JZSignalEvent
JZSignalEvent::JZSignalEvent()
    :JZEvent(JZEvent_signal)
{
}

JZSignalEvent::~JZSignalEvent()
{
}

//JZBaseSignal
JZBaseSignal::JZBaseSignal()
{
}

JZBaseSignal::~JZBaseSignal()
{
}

void JZBaseSignal::pushSender(JZObjectPointer& recv, JZObject *sender)
{
    if(recv)
        recv.object()->pushSender(sender);
}

void JZBaseSignal::popSender(JZObjectPointer& recv)
{
    if(recv)
        recv.object()->popSender();
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

void JZBaseSignal::postEvent(JZObject *recvier, JZSignalEvent *event)
{
    if(recvier)
        jzApp->postEvent(recvier, event);
    else
    {
        JZThread* t = (JZThread*)currentThread();
        
        JZEventInfo e(t, event);
        t->postEvent(e);
    }
}

void JZBaseSignal::postEventAndWait(JZObject *recvier, JZSignalEvent *event)
{
    if (recvier)
        jzApp->postEventAndWait(recvier, event);
    else
    {
        JZ_ASSERT(0);
    }
}