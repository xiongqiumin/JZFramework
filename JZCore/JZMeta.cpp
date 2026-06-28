#include "JZMeta.h"
#include "JZMutex.h"

//JZMetaBase
JZMetaBase::~JZMetaBase()
{
}

//JZMetaRegister
class JZMetaRegister::JZMetaRegisterImpl
{
public:
    JZMetaRegisterImpl()
    {
        m_id = JZ_META_USER;
    }

    int genId()
    {
        JZMutexLocker locker(&m_mutex);
        return m_id++;
    }

protected:
    int m_id;
    JZMutex m_mutex;
};

JZMetaRegister *JZMetaRegister::instance()
{
    static JZMetaRegister inst;
    return &inst;
}

JZMetaRegister::JZMetaRegister()
{
    impl = new JZMetaRegisterImpl();
}

JZMetaRegister::~JZMetaRegister()
{
    delete impl;
}

int JZMetaRegister::genId()
{
    return impl->genId();
}