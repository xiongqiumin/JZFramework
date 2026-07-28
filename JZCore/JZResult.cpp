#include "JZResult.h"

// ---- JZResult<void> 特化实现 ----

JZResult<void> JZResult<void>::Ok()
{
    JZResult<void> ret;
    ret.m_isOk = true;
    ret.m_code = 0;
    return ret;
}

JZResult<void> JZResult<void>::Error(const std::string& error_msg)
{
    return Error(1, error_msg);
}

JZResult<void> JZResult<void>::Error(int code, const std::string& error_msg)
{
    JZResult<void> ret;
    ret.m_isOk = false;
    ret.m_code = code;
    ret.m_error = error_msg;
    return ret;
}

JZResult<void>::JZResult(JZResultStatus error)
{
    m_code = error.code;
    m_error = error.message;
    m_isOk = false;
}

JZResult<void>::JZResult()
{
    m_isOk = false;
    m_code = 1;
}

bool JZResult<void>::isOk() const
{
    return m_isOk;
}

bool JZResult<void>::isError() const
{
    return !m_isOk;
}

JZResult<void>::operator bool() const
{
    return m_isOk;
}

int JZResult<void>::errorCode() const
{
    return m_code;
}

const std::string& JZResult<void>::errorMessage() const
{
    assert(isError() && "JZResult<void>::errorMessage() called on Ok");
    return m_error;
}

JZResultStatus JZResult<void>::status() const
{
    JZResultStatus err;
    err.code = m_code;
    err.message = m_error;
    return err;
}
