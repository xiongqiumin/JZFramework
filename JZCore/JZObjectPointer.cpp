#include "JZObjectPointer.h"
#include "JZObject.h"

//JZObjectPointer
JZObject* JZObjectPointer::object() const
{
    if (!m_data)
        return nullptr;

    return m_data->object;
}

JZObjectPointer::operator bool() const
{
    return object() != nullptr;
}

bool JZObjectPointer::operator==(const JZObjectPointer& other) const
{
    if (m_data && other.m_data)
        return m_data->object == other.m_data->object;
    else if (!m_data && !other.m_data)
        return true;
    else
        return false;
}

bool JZObjectPointer::operator!=(const JZObjectPointer& other) const
{
    return !(*this == other);
}

JZObject* JZObjectPointer::operator->() const
{
    return object();
}