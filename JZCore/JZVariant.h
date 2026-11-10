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

// ⚠ 下面全部是**模板显式特化**, 定义在 JZVariant.cpp 里(非 header-only)。
//   类本身带 JZFRAMEWORK_API 并不会顺带导出这些特化 ⇒ 动态库模式下必须逐个标注,
//   否则使用方(JZSlot/JZSignal 里的 fromValue<T>/value<T>/valueRef<T> 调用点)链接期 LNK2019。
// fromValue 特化声明
template <>
JZFRAMEWORK_API JZVariant JZVariant::fromValue(const bool &t);
template <>
JZFRAMEWORK_API JZVariant JZVariant::fromValue(const int &t);
template <>
JZFRAMEWORK_API JZVariant JZVariant::fromValue(const int64_t &t);
template <>
JZFRAMEWORK_API JZVariant JZVariant::fromValue(const float &t);
template <>
JZFRAMEWORK_API JZVariant JZVariant::fromValue(const double &t);
template <>
JZFRAMEWORK_API JZVariant JZVariant::fromValue(void *const &t);
template <>
JZFRAMEWORK_API JZVariant JZVariant::fromValue(const int16_t &t);
template <>
JZFRAMEWORK_API JZVariant JZVariant::fromValue(const uint16_t &t);
template <>
JZFRAMEWORK_API JZVariant JZVariant::fromValue(const unsigned int &t);
template <>
JZFRAMEWORK_API JZVariant JZVariant::fromValue(const uint64_t &t);

// value 特化声明
template <>
JZFRAMEWORK_API bool JZVariant::value<bool>() const;
template <>
JZFRAMEWORK_API int JZVariant::value<int>() const;
template <>
JZFRAMEWORK_API int64_t JZVariant::value<int64_t>() const;
template <>
JZFRAMEWORK_API float JZVariant::value<float>() const;
template <>
JZFRAMEWORK_API double JZVariant::value<double>() const;
template <>
JZFRAMEWORK_API void *JZVariant::value<void *>() const;
template <>
JZFRAMEWORK_API int16_t JZVariant::value<int16_t>() const;
template <>
JZFRAMEWORK_API uint16_t JZVariant::value<uint16_t>() const;
template <>
JZFRAMEWORK_API unsigned int JZVariant::value<unsigned int>() const;
template <>
JZFRAMEWORK_API uint64_t JZVariant::value<uint64_t>() const;

//==================== valueRef 非const 可读写版本声明 ====================
template <>
JZFRAMEWORK_API bool& JZVariant::valueRef<bool>();
template <>
JZFRAMEWORK_API int& JZVariant::valueRef<int>();
template <>
JZFRAMEWORK_API int64_t& JZVariant::valueRef<int64_t>();
template <>
JZFRAMEWORK_API float& JZVariant::valueRef<float>();
template <>
JZFRAMEWORK_API double& JZVariant::valueRef<double>();
template <>
JZFRAMEWORK_API void*& JZVariant::valueRef<void*>();
template <>
JZFRAMEWORK_API int16_t& JZVariant::valueRef<int16_t>();
template <>
JZFRAMEWORK_API uint16_t& JZVariant::valueRef<uint16_t>();
template <>
JZFRAMEWORK_API unsigned int& JZVariant::valueRef<unsigned int>();
template <>
JZFRAMEWORK_API uint64_t& JZVariant::valueRef<uint64_t>();

//==================== valueRef const 只读版本声明 ====================
template <>
JZFRAMEWORK_API const bool& JZVariant::valueRef<bool>() const;
template <>
JZFRAMEWORK_API const int& JZVariant::valueRef<int>() const;
template <>
JZFRAMEWORK_API const int64_t& JZVariant::valueRef<int64_t>() const;
template <>
JZFRAMEWORK_API const float& JZVariant::valueRef<float>() const;
template <>
JZFRAMEWORK_API const double& JZVariant::valueRef<double>() const;
template <>
JZFRAMEWORK_API void* const& JZVariant::valueRef<void*>() const;
template <>
JZFRAMEWORK_API const int16_t& JZVariant::valueRef<int16_t>() const;
template <>
JZFRAMEWORK_API const uint16_t& JZVariant::valueRef<uint16_t>() const;
template <>
JZFRAMEWORK_API const unsigned int& JZVariant::valueRef<unsigned int>() const;
template <>
JZFRAMEWORK_API const uint64_t& JZVariant::valueRef<uint64_t>() const;

JZDataStream& operator<<(JZDataStream& s, const JZVariant& vec);
JZDataStream& operator>>(JZDataStream& s, JZVariant& vec);

#endif