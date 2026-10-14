#ifndef JZ_META_H_
#define JZ_META_H_
#include <string>
#include <inttypes.h>
#include "JZGlobal.h"

constexpr int JZ_META_NONE = 0;
constexpr int JZ_META_USER = 256;

class JZFRAMEWORK_API JZMetaType 
{
public:
    enum {
        None,
        Bool,
        Int16,
        UInt16,
        Int,
        UInt,
        Int64,
        UInt64,
        Float,
        Double,
        Pointer,

        PodEnd,

        String,
        ByteArray,
        VariantMap,
        VariantList,
        NetPackPtr,
    };
};

class JZFRAMEWORK_API JZMetaBase
{
public:
    virtual ~JZMetaBase();

    virtual void *ptr() = 0;
    virtual JZMetaBase *clone() = 0;
};

template<class T>
class JZMeta : public JZMetaBase
{
public:
    virtual ~JZMeta()
    {

    }

    virtual void *ptr()
    {
        return &data;
    }

    virtual JZMetaBase *clone()
    {
        JZMeta *t = new JZMeta();
        t->data = data;
        return t;
    };
    T data;
};

class JZFRAMEWORK_API JZMetaRegister
{
public:
    static JZMetaRegister *instance();    
    int genId();

protected:
    class JZMetaRegisterImpl;
    JZ_DISABLE_COPY(JZMetaRegister)

    JZMetaRegister();
    ~JZMetaRegister();

    JZMetaRegisterImpl *impl;
};

template <class T>
struct JZMetaTypeId
{
    enum { Defined = 0 };
    static int jz_metatype_id() 
    {
        return JZ_META_NONE;
    }
};

#define JZ_DECLARE_METATYPE_INTERNAL(TYPE,id)  \
    template <>                                \
    struct JZMetaTypeId< TYPE >                \
    {                                          \
        enum { Defined = 1 };                  \
        static int jz_metatype_id()            \
        {                                      \
            return id;                         \
        }                                      \
    };

#define JZ_DECLARE_METATYPE(TYPE)                                   \
    template <>                                                     \
    struct JZMetaTypeId< TYPE >                                     \
    {                                                               \
        enum { Defined = 1 };                                       \
        static int jz_metatype_id()                                 \
        {                                                           \
            static int metatype_id = JZMetaRegister::instance()->genId();  \
            return metatype_id;                                            \
        }                                                           \
    };

template<class T>
int jzMetaTypeId()
{
    static_assert(JZMetaTypeId<T>::Defined, "please use JZ_DECLARE_METATYPE defined first");
    return JZMetaTypeId<T>::jz_metatype_id();
}

JZ_DECLARE_METATYPE_INTERNAL(bool, JZMetaType::Bool);
JZ_DECLARE_METATYPE_INTERNAL(int16_t, JZMetaType::Int16);
JZ_DECLARE_METATYPE_INTERNAL(uint16_t, JZMetaType::UInt16);
JZ_DECLARE_METATYPE_INTERNAL(int, JZMetaType::Int);
JZ_DECLARE_METATYPE_INTERNAL(unsigned int, JZMetaType::UInt);
JZ_DECLARE_METATYPE_INTERNAL(int64_t, JZMetaType::Int64);
JZ_DECLARE_METATYPE_INTERNAL(uint64_t, JZMetaType::UInt64);
JZ_DECLARE_METATYPE_INTERNAL(float, JZMetaType::Float);
JZ_DECLARE_METATYPE_INTERNAL(double, JZMetaType::Double);
JZ_DECLARE_METATYPE_INTERNAL(void*, JZMetaType::Pointer);
JZ_DECLARE_METATYPE_INTERNAL(std::string, JZMetaType::String);

#endif // !JZ_META_H_