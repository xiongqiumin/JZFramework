#ifndef JZ_OBJECT_POINTEER_H_
#define JZ_OBJECT_POINTEER_H_

#include <memory>
#include "JZGlobal.h"

class JZObject;
class JZObjectPointerData
{
public:
    JZObject* object;
};

class JZObjectPointer
{
public:
    JZObject* object() const;
    operator bool() const;
    bool operator==(const JZObjectPointer &other) const;
    bool operator!=(const JZObjectPointer &other) const;

    JZObject* operator->() const;

protected:
    friend JZObject;

    std::shared_ptr<JZObjectPointerData> m_data;
};

#endif