#ifndef JZ_OBJECT_POINTER_H_
#define JZ_OBJECT_POINTER_H_

#include <memory>
#include "JZGlobal.h"

class JZObject;
class JZDebug;

class JZObjectPointerData
{
public:
    JZObject* object;
};

class JZObjectPointerGuard;
class JZObjectPointer
{
public:
    operator bool() const;
    JZObject* object() const;

protected:
    friend JZObject;
    friend JZObjectPointerGuard;

    std::shared_ptr<JZObjectPointerData> data;
};

class JZObjectPointerGuard
{
public:
    JZObjectPointerGuard(JZObjectPointer *pointer);
    ~JZObjectPointerGuard();    

    void relock();
    void unlock();

    JZObject* object();

protected:
    JZObjectPointer *m_pointer;
    bool m_isLocked;
};


JZDebug operator<<(JZDebug d, const JZObjectPointer& p);

#endif