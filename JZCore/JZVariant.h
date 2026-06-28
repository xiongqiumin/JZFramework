#ifndef JZ_VARIIANT_H_
#define JZ_VARIIANT_H_

#include <string>
#include <string.h>
#include "JZGlobal.h"
#include "JZMeta.h"
#include "JZVariant.h"
#include "JZByteArray.h"

//JZVariant
class JZVariant
{
public:
    static const JZVariant &None();

    JZVariant();     
    JZVariant(bool b);
    JZVariant(int i);
    JZVariant(int64_t i64);
    JZVariant(float f);
    JZVariant(double d);
    JZVariant(const char *str);
    JZVariant(const std::string &str);
    ~JZVariant();

    JZVariant(const JZVariant &other);
    void operator=(const JZVariant &other);

    int typeId() const;
    bool isValid() const;

    bool toBool() const;
    int toInt() const;
    int64_t toInt64() const;
    float toFloat() const;
    double toDouble() const;
    std::string toString() const;
    JZByteArray toByteArray() const;

    template <class T>
    static JZVariant fromValue(const T &t)
    {
        JZVariant v;
        v.m_type = jzMetaTypeId<T>();

        JZ_ASSERT(v.m_type >= JZMetaType::PodEnd);
        
        JZMeta<T> *meta = new JZMeta<T>();
        meta->data = t;
        v.m_data.meta = meta;
        return v;
    }

    template <class T>
    T value() const
    {
        if (m_type == JZMetaType::None)
            return T();

        JZ_ASSERT(m_type == jzMetaTypeId<T>());
        JZ_ASSERT(m_type >= JZMetaType::PodEnd);
        
        auto meta = (JZMeta<T>*)m_data.meta;
        return meta->data;
    }

    void *data();
    const void *data() const;
    void release();

protected:
    union Data {
        bool b;
        int i;
        int64_t i64;
        float f;
        double d;
        void *ptr;
        JZMetaBase *meta;
    };

    int m_type;
    Data m_data;    
};

typedef std::vector<JZVariant> JZVariantList;

// fromValue 特化声明
template <>
JZVariant JZVariant::fromValue(const bool &t);
template <>
JZVariant JZVariant::fromValue(const int &t);
template <>
JZVariant JZVariant::fromValue(const int64_t &t);
template <>
JZVariant JZVariant::fromValue(const float &t);
template <>
JZVariant JZVariant::fromValue(const double &t);
template <>
JZVariant JZVariant::fromValue(void *const &t);

// value 特化声明
template <>
bool JZVariant::value<bool>() const;
template <>
int JZVariant::value<int>() const;
template <>
int64_t JZVariant::value<int64_t>() const;
template <>
float JZVariant::value<float>() const;
template <>
double JZVariant::value<double>() const;
template <>
void *JZVariant::value<void *>() const;

#endif