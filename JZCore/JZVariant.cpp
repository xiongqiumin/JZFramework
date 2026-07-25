#include "JZVariant.h"
#include "JZDataStream.h"

//JZVariant
const JZVariant &JZVariant::None()
{
    static JZVariant none;
    return none;
}

template <>
JZVariant JZVariant::fromValue(const bool &t)
{
    return JZVariant(t);
}

template <>
JZVariant JZVariant::fromValue(const int &t)
{
    return JZVariant(t);
}

template <>
JZVariant JZVariant::fromValue(const int64_t &t)
{
    return JZVariant(t);
}

template <>
JZVariant JZVariant::fromValue(const float &t)
{
    return JZVariant(t);
}

template <>
JZVariant JZVariant::fromValue(const double &t)
{
    return JZVariant(t);
}

template <>
JZVariant JZVariant::fromValue(void *const &t)
{
    JZVariant v;
    memset(&v.m_data, 0, sizeof(v.m_data));
    v.m_data.ptr = t;
    return v;
}

template <>
JZVariant JZVariant::fromValue(const int16_t &t)
{
    return JZVariant(t);
}

template <>
JZVariant JZVariant::fromValue(const uint16_t &t)
{
    return JZVariant(t);
}

template <>
JZVariant JZVariant::fromValue(const unsigned int &t)
{
    return JZVariant(t);
}

template <>
JZVariant JZVariant::fromValue(const uint64_t &t)
{
    return JZVariant(t);
}

JZVariant::JZVariant()
{
    m_type = JZMetaType::None;    
    memset(&m_data, 0, sizeof(m_data));
}

JZVariant::JZVariant(bool b)
{
    m_type = JZMetaType::Bool;
    memset(&m_data, 0, sizeof(m_data));   // 保证 POD union 内存在各类型下一致
    m_data.b = b;
}

JZVariant::JZVariant(int i)
{
    m_type = JZMetaType::Int;
    memset(&m_data, 0, sizeof(m_data));
    m_data.i = i;
}

JZVariant::JZVariant(int64_t i64)
{
    m_type = JZMetaType::Int64;
    memset(&m_data, 0, sizeof(m_data));
    m_data.i64 = i64;
}

JZVariant::JZVariant(float f)
{
    m_type = JZMetaType::Float;
    memset(&m_data, 0, sizeof(m_data));
    m_data.f = f;
}

JZVariant::JZVariant(double d)
{
    m_type = JZMetaType::Double;
    memset(&m_data, 0, sizeof(m_data));
    m_data.d = d;
}

JZVariant::JZVariant(const char *cstr)
{
    m_type = JZMetaType::String;

    auto meta = new JZMeta<std::string>();
    meta->data = std::string(cstr);    
    m_data.meta = meta; 
}

JZVariant::JZVariant(const std::string &str)
{
    m_type = JZMetaType::String;

    auto meta = new JZMeta<std::string>();
    meta->data = str;
    m_data.meta = meta;
}

JZVariant::JZVariant(int16_t i16)
{
    m_type = JZMetaType::Int16;
    memset(&m_data, 0, sizeof(m_data));
    m_data.i16 = i16;
}

JZVariant::JZVariant(uint16_t ui16)
{
    m_type = JZMetaType::UInt16;
    memset(&m_data, 0, sizeof(m_data));
    m_data.ui16 = ui16;
}

JZVariant::JZVariant(unsigned int ui)
{
    m_type = JZMetaType::UInt;
    memset(&m_data, 0, sizeof(m_data));
    m_data.ui = ui;
}

JZVariant::JZVariant(uint64_t ui64)
{
    m_type = JZMetaType::UInt64;
    memset(&m_data, 0, sizeof(m_data));
    m_data.ui64 = ui64;
}

JZVariant::JZVariant(const JZVariantMap &m)
{
    m_type = JZMetaType::VariantMap;

    auto meta = new JZMeta<JZVariantMap>();
    meta->data = m;
    m_data.meta = meta;
}

JZVariant::JZVariant(const JZVariantList &l)
{
    m_type = JZMetaType::VariantList;

    auto meta = new JZMeta<JZVariantList>();
    meta->data = l;
    m_data.meta = meta;
}

JZVariant::JZVariant(JZVariant &&other) noexcept
{
    m_type = other.m_type;
    m_data = other.m_data;
    other.m_type = JZMetaType::None;
    memset(&other.m_data, 0, sizeof(other.m_data));
}

JZVariant::~JZVariant()
{
    release();
}

JZVariant::JZVariant(const JZVariant &other)
{
    m_type = other.m_type;
    if (m_type < JZMetaType::PodEnd)
    {
        memcpy(&m_data, &other.m_data, sizeof(Data));
    }
    else
    {
        m_data.meta = other.m_data.meta->clone();
    }
}

void JZVariant::operator=(const JZVariant &other)
{
    if (this == &other)
        return;

    release();
    m_type = other.m_type;
    if (m_type < JZMetaType::PodEnd)
    {
        memcpy(&m_data,&other.m_data,sizeof(Data));
    }
    else
    {        
        m_data.meta = other.m_data.meta->clone();
    }
}

void JZVariant::operator=(JZVariant && other) noexcept
{
    if (this == &other)
        return;

    release();
    m_type = other.m_type;
    m_data = other.m_data;
    other.m_type = JZMetaType::None;
    memset(&other.m_data, 0, sizeof(other.m_data));
}

bool JZVariant::operator==(const JZVariant& other) const
{
    if (this == &other)
        return true;

    if (m_type != other.m_type)
        return false;

    // POD 类型在构造时已整体清零 union, 内存一致, 可直接按位比较
    if (m_type < JZMetaType::PodEnd)
        return memcmp(&m_data, &other.m_data, sizeof(Data)) == 0;

    return false;   // 非 POD(字符串/容器等) 不按位比较
}

bool JZVariant::operator!=(const JZVariant& other) const
{
    return !(*this == other);
}

int JZVariant::typeId() const
{
    return m_type;
}

bool JZVariant::isValid() const
{ 
    return m_type != JZMetaType::None; 
}

bool JZVariant::toBool() const
{
    return value<bool>();
}

int JZVariant::toInt() const
{
    return value<int>();
}

int64_t JZVariant::toInt64() const
{
    return value<int64_t>();
}

float JZVariant::toFloat() const
{
    return value<float>();
}

double JZVariant::toDouble() const
{
    return value<double>();
}

std::string JZVariant::toString() const
{
    return value<std::string>();
}

JZByteArray JZVariant::toByteArray() const
{
    return value<JZByteArray>();
}

JZVariantMap JZVariant::toMap() const
{
    return value<JZVariantMap>();
}

JZVariantList JZVariant::toList() const
{
    return value<JZVariantList>();
}

void *JZVariant::data()
{
    if (m_type < JZMetaType::PodEnd)
        return &m_data;
    else
        return m_data.meta->ptr();
}

const void *JZVariant::data() const
{
    if (m_type < JZMetaType::PodEnd)
        return &m_data;
    else
        return m_data.meta->ptr();
}

void JZVariant::release()
{    
    if (m_type >= JZMetaType::PodEnd)
    {        
        delete m_data.meta;
        m_data.meta = nullptr;
    }
    m_type = JZMetaType::None;
    memset(&m_data, 0, sizeof(Data));
}

template <>
bool JZVariant::value<bool>() const
{
    JZ_ASSERT(m_type == JZMetaType::Bool);
    return m_data.b;
}

template <>
int JZVariant::value<int>() const
{
    JZ_ASSERT(m_type == JZMetaType::Int);
    return m_data.i;
}

template <>
int64_t JZVariant::value<int64_t>() const
{
    JZ_ASSERT(m_type == JZMetaType::Int64);
    return m_data.i64;
}

template <>
float JZVariant::value<float>() const
{
    JZ_ASSERT(m_type == JZMetaType::Float);
    return m_data.f;
}

template <>
double JZVariant::value<double>() const
{
    JZ_ASSERT(m_type == JZMetaType::Double);
    return m_data.d;
}

template <>
void* JZVariant::value<void*>() const
{
    JZ_ASSERT(m_type == JZMetaType::Pointer);
    return m_data.ptr;
}

template <>
int16_t JZVariant::value<int16_t>() const
{
    JZ_ASSERT(m_type == JZMetaType::Int16);
    return m_data.i16;
}

template <>
uint16_t JZVariant::value<uint16_t>() const
{
    JZ_ASSERT(m_type == JZMetaType::UInt16);
    return m_data.ui16;
}

template <>
unsigned int JZVariant::value<unsigned int>() const
{
    JZ_ASSERT(m_type == JZMetaType::UInt);
    return m_data.ui;
}

template <>
uint64_t JZVariant::value<uint64_t>() const
{
    JZ_ASSERT(m_type == JZMetaType::UInt64);
    return m_data.ui64;
}

template <>
bool& JZVariant::valueRef<bool>()
{
    JZ_ASSERT(m_type == JZMetaType::Bool);
    return m_data.b;
}

template <>
int& JZVariant::valueRef<int>()
{
    JZ_ASSERT(m_type == JZMetaType::Int);
    return m_data.i;
}

template <>
int64_t& JZVariant::valueRef<int64_t>()
{
    JZ_ASSERT(m_type == JZMetaType::Int64);
    return m_data.i64;
}

template <>
float& JZVariant::valueRef<float>()
{
    JZ_ASSERT(m_type == JZMetaType::Float);
    return m_data.f;
}

template <>
double& JZVariant::valueRef<double>()
{
    JZ_ASSERT(m_type == JZMetaType::Double);
    return m_data.d;
}

template <>
void*& JZVariant::valueRef<void*>()
{
    JZ_ASSERT(m_type == JZMetaType::Pointer);
    return m_data.ptr;
}

template <>
int16_t& JZVariant::valueRef<int16_t>()
{
    JZ_ASSERT(m_type == JZMetaType::Int16);
    return m_data.i16;
}

template <>
uint16_t& JZVariant::valueRef<uint16_t>()
{
    JZ_ASSERT(m_type == JZMetaType::UInt16);
    return m_data.ui16;
}

template <>
unsigned int& JZVariant::valueRef<unsigned int>()
{
    JZ_ASSERT(m_type == JZMetaType::UInt);
    return m_data.ui;
}

template <>
uint64_t& JZVariant::valueRef<uint64_t>()
{
    JZ_ASSERT(m_type == JZMetaType::UInt64);
    return m_data.ui64;
}

template <>
const bool& JZVariant::valueRef<bool>() const
{
    JZ_ASSERT(m_type == JZMetaType::Bool);
    return m_data.b;
}

template <>
const int& JZVariant::valueRef<int>() const
{
    JZ_ASSERT(m_type == JZMetaType::Int);
    return m_data.i;
}

template <>
const int64_t& JZVariant::valueRef<int64_t>() const
{
    JZ_ASSERT(m_type == JZMetaType::Int64);
    return m_data.i64;
}

template <>
const float& JZVariant::valueRef<float>() const
{
    JZ_ASSERT(m_type == JZMetaType::Float);
    return m_data.f;
}

template <>
const double& JZVariant::valueRef<double>() const
{
    JZ_ASSERT(m_type == JZMetaType::Double);
    return m_data.d;
}

template <>
void* const& JZVariant::valueRef<void*>() const
{
    JZ_ASSERT(m_type == JZMetaType::Pointer);
    return m_data.ptr;
}

template <>
const int16_t& JZVariant::valueRef<int16_t>() const
{
    JZ_ASSERT(m_type == JZMetaType::Int16);
    return m_data.i16;
}

template <>
const uint16_t& JZVariant::valueRef<uint16_t>() const
{
    JZ_ASSERT(m_type == JZMetaType::UInt16);
    return m_data.ui16;
}

template <>
const unsigned int& JZVariant::valueRef<unsigned int>() const
{
    JZ_ASSERT(m_type == JZMetaType::UInt);
    return m_data.ui;
}

template <>
const uint64_t& JZVariant::valueRef<uint64_t>() const
{
    JZ_ASSERT(m_type == JZMetaType::UInt64);
    return m_data.ui64;
}

JZDataStream& operator<<(JZDataStream& s, const JZVariant& v)
{
    s << (int32_t)v.typeId();
    switch (v.typeId())
    {
        case JZMetaType::Bool:      s << (int8_t)(v.toBool() ? 1 : 0); break;
        case JZMetaType::Int:       s << (int32_t)v.toInt(); break;
        case JZMetaType::Int64:     s << (int64_t)v.toInt64(); break;
        case JZMetaType::Float:     s << v.toFloat(); break;
        case JZMetaType::Double:    s << v.toDouble(); break;
        case JZMetaType::Int16:     s << (int16_t)v.value<int16_t>(); break;
        case JZMetaType::UInt:      s << (uint32_t)v.value<unsigned int>(); break;
        case JZMetaType::UInt64:    s << (uint64_t)v.value<uint64_t>(); break;
        case JZMetaType::String:    s << v.toString(); break;
        case JZMetaType::ByteArray: s << v.toByteArray(); break;
        case JZMetaType::VariantMap: {
            const JZVariantMap &m = v.valueRef<JZVariantMap>();
            s << (int32_t)m.size();
            for (auto &kv : m) { s << kv.first; s << kv.second; }
            break;
        }
        case JZMetaType::VariantList: {
            const JZVariantList &l = v.valueRef<JZVariantList>();
            s << (int32_t)l.size();
            for (auto &e : l) s << e;
            break;
        }
        default: {
            JZ_ASSERT(0);
            break;
        }
    }
    return s;
}

JZDataStream& operator>>(JZDataStream& s, JZVariant& v)
{
    int32_t t = 0;
    s >> t;
    switch (t)
    {
        case JZMetaType::Bool:      { int8_t b; s >> b; v = JZVariant(b != 0); } break;
        case JZMetaType::Int:       { int32_t i; s >> i; v = JZVariant((int)i); } break;
        case JZMetaType::Int64:     { int64_t i; s >> i; v = JZVariant(i); } break;
        case JZMetaType::Float:     { float f; s >> f; v = JZVariant(f); } break;
        case JZMetaType::Double:    { double d; s >> d; v = JZVariant(d); } break;
        case JZMetaType::Int16:     { int16_t i; s >> i; v = JZVariant(i); } break;
        case JZMetaType::UInt:      { uint32_t i; s >> i; v = JZVariant((unsigned int)i); } break;
        case JZMetaType::UInt64:    { uint64_t i; s >> i; v = JZVariant(i); } break;
        case JZMetaType::String:    { std::string st; s >> st; v = JZVariant(st); } break;
        case JZMetaType::ByteArray: { JZByteArray ba; s >> ba; v = JZVariant::fromValue(ba); } break;
        case JZMetaType::VariantMap: {
            int32_t n; s >> n;
            JZVariantMap m;
            for (int32_t i = 0; i < n; i++) {
                std::string k; s >> k;
                JZVariant val; s >> val;
                m[k] = val;
            }
            v = JZVariant(m);
            break;
        }
        case JZMetaType::VariantList: {
            int32_t n; s >> n;
            JZVariantList l;
            l.resize(n);
            for (int32_t i = 0; i < n; i++) {
                JZVariant val; s >> val;
                l[i] = val;
            }
            v = JZVariant(l);
            break;
        }
        default: {
            JZ_ASSERT(0);
            break;
        }
    }
    return s;
}