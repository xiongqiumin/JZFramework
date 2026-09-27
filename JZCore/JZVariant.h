#ifndef JZ_VARIANT_H_
#define JZ_VARIANT_H_
#include <map>
#include <string>
#include <string.h>
#include "JZGlobal.h"
#include "JZMeta.h"
#include "JZByteArray.h"

//JZVariant
class JZDataStream;
class JZFRAMEWORK_API JZVariant
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
    JZVariant(int16_t i16);
    JZVariant(uint16_t ui16);
    JZVariant(unsigned int ui);
    JZVariant(uint64_t ui64);
    JZVariant(const std::map<std::string, JZVariant> &m);
    JZVariant(const std::vector<JZVariant>&l);
    JZVariant(JZVariant&&) noexcept;
    ~JZVariant();

    JZVariant(const JZVariant &other);
    void operator=(const JZVariant &other);
    void operator=(JZVariant&&) noexcept;

    bool operator==(const JZVariant& other) const;
    bool operator!=(const JZVariant& other) const;

    int typeId() const;
    bool isValid() const;

    bool toBool() const;
    int toInt() const;
    int64_t toInt64() const;
    float toFloat() const;
    double toDouble() const;
    std::string toString() const;
    JZByteArray toByteArray() const;
    std::map<std::string, JZVariant> toMap() const;
    std::vector<JZVariant> toList() const;

    template <class T>
    static JZVariant fromValue(const T &t)
    {
        JZVariant v;
        v.m_type = jzMetaTypeId<T>();

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

        auto meta = (JZMeta<T>*)m_data.meta;
        return meta->data;
    }

    template <class T>
    T& valueRef()
    {
        JZ_ASSERT(m_type == jzMetaTypeId<T>());

        auto meta = (JZMeta<T>*)m_data.meta;
        return meta->data;
    }

    template <class T>
    const T& valueRef() const
    {
        JZ_ASSERT(m_type == jzMetaTypeId<T>());

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
        unsigned int ui;
        int64_t i64;
        uint64_t ui64;
        int16_t i16;
        uint16_t ui16;
        float f;
        double d;
        void *ptr;
        JZMetaBase *meta;
    };

    int m_type;
    Data m_data;    
};

typedef std::vector<JZVariant> JZVariantList;
typedef std::map<std::string,JZVariant> JZVariantMap;

// JZVariantMap / JZVariantList 作为 JZVariant 的内置值类型(固定元类型 id, 随 JZVariant 一起序列化/拷贝)
JZ_DECLARE_METATYPE_INTERNAL(JZVariantMap,  JZMetaType::VariantMap)
JZ_DECLARE_METATYPE_INTERNAL(JZVariantList, JZMetaType::VariantList)

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
template <>
JZVariant JZVariant::fromValue(const int16_t &t);
template <>
JZVariant JZVariant::fromValue(const uint16_t &t);
template <>
JZVariant JZVariant::fromValue(const unsigned int &t);
template <>
JZVariant JZVariant::fromValue(const uint64_t &t);

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
template <>
int16_t JZVariant::value<int16_t>() const;
template <>
uint16_t JZVariant::value<uint16_t>() const;
template <>
unsigned int JZVariant::value<unsigned int>() const;
template <>
uint64_t JZVariant::value<uint64_t>() const;

//==================== valueRef 非const 可读写版本声明 ====================
template <>
bool& JZVariant::valueRef<bool>();
template <>
int& JZVariant::valueRef<int>();
template <>
int64_t& JZVariant::valueRef<int64_t>();
template <>
float& JZVariant::valueRef<float>();
template <>
double& JZVariant::valueRef<double>();
template <>
void*& JZVariant::valueRef<void*>();
template <>
int16_t& JZVariant::valueRef<int16_t>();
template <>
uint16_t& JZVariant::valueRef<uint16_t>();
template <>
unsigned int& JZVariant::valueRef<unsigned int>();
template <>
uint64_t& JZVariant::valueRef<uint64_t>();

//==================== valueRef const 只读版本声明 ====================
template <>
const bool& JZVariant::valueRef<bool>() const;
template <>
const int& JZVariant::valueRef<int>() const;
template <>
const int64_t& JZVariant::valueRef<int64_t>() const;
template <>
const float& JZVariant::valueRef<float>() const;
template <>
const double& JZVariant::valueRef<double>() const;
template <>
void* const& JZVariant::valueRef<void*>() const;
template <>
const int16_t& JZVariant::valueRef<int16_t>() const;
template <>
const uint16_t& JZVariant::valueRef<uint16_t>() const;
template <>
const unsigned int& JZVariant::valueRef<unsigned int>() const;
template <>
const uint64_t& JZVariant::valueRef<uint64_t>() const;

JZDataStream& operator<<(JZDataStream& s, const JZVariant& vec);
JZDataStream& operator>>(JZDataStream& s, JZVariant& vec);

#endif