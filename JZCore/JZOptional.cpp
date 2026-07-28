#include "JZOptional.h"

// ---- JZOptional<void> 特化实现 ----

JZOptional<void> JZOptional<void>::None()
{
    return JZOptional<void>();
}

JZOptional<void> JZOptional<void>::Some()
{
    JZOptional<void> ret;
    ret.m_hasValue = true;
    return ret;
}

JZOptional<void>::JZOptional() : m_hasValue(false)
{
}

JZOptional<void>::operator bool() const noexcept
{
    return isSome();
}

bool JZOptional<void>::isSome() const
{
    return m_hasValue;
}

bool JZOptional<void>::isNone() const
{
    return !m_hasValue;
}

void JZOptional<void>::reset()
{
    m_hasValue = false;
}