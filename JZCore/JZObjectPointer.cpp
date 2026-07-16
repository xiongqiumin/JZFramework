#include "JZObjectPointer.h"
#include "JZObject.h"
#include "JZApplication.h"

//JZObjectPointer
JZObjectPointer::operator bool() const
{
    if(!data)
        return false;

    return data->object != nullptr;
}

JZObject* JZObjectPointer::object() const
{
    if (!data)
        return nullptr;
        
    return data->object;
}

//JZObjectPointerGuard
JZObjectPointerGuard::JZObjectPointerGuard(JZObjectPointer *pointer)
{
    m_pointer = pointer;
    relock();
}

JZObjectPointerGuard::~JZObjectPointerGuard()
{
    unlock();
}   

void JZObjectPointerGuard::relock()
{
    m_isLocked = true;
    jzApp->globalLock();
}

void JZObjectPointerGuard::unlock()
{
    if (m_isLocked)
    {
        jzApp->globalUnlock();
        m_isLocked = false;
    }
}

JZObject* JZObjectPointerGuard::object()
{
    if (!m_pointer->data)
        return nullptr;
    return m_pointer->data->object;
}