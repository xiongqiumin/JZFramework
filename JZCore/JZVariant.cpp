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
    v.m_data.ptr = t;
    return v;
}

JZVariant::JZVariant()
{
    m_type = JZMetaType::None;    
    memset(&m_data, 0, sizeof(m_data));
}

JZVariant::JZVariant(bool b)
{
    m_type = JZMetaType::Bool;
    m_data.b = b;
}

JZVariant::JZVariant(int i)
{
    m_type = JZMetaType::Int;
    m_data.i = i;
}

JZVariant::JZVariant(int64_t i64)
{
    m_type = JZMetaType::Int64;
    m_data.i64 = i64;
}

JZVariant::JZVariant(float f)
{
    m_type = JZMetaType::Float;
    m_data.f = f;
}

JZVariant::JZVariant(double d)
{
    m_type = JZMetaType::Double;
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

    m_type = other.m_type;
    m_data = other.m_data;
    other.m_type = JZMetaType::None;
    memset(&other.m_data, 0, sizeof(other.m_data));
}

bool JZVariant::operator==(const JZVariant& other) const
{
    if (this == &other)
        return true;

    if (m_type < JZMetaType::PodEnd && other.m_type < JZMetaType::PodEnd)
    {
        bool ret = (memcmp(&m_data, &other.m_data, sizeof(Data)) == 0);
        return ret;
    }
    else
    {
        return false;
    }
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
        case JZMetaType::String:    s << v.toString(); break;
        case JZMetaType::ByteArray: s << v.toByteArray(); break;
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
        case JZMetaType::String:    { std::string st; s >> st; v = JZVariant(st); } break;
        case JZMetaType::ByteArray: { JZByteArray ba; s >> ba; v = JZVariant::fromValue(ba); } break;
        default: {
            JZ_ASSERT(0);
            break;
        }
    }
    return s;
}