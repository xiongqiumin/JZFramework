#include "JZSimPack.h"
#include "JZDataStream.h"
#include "JZMeta.h"

// JZVariantMap / JZVariantList 现在是 JZVariant 的内置值类型(见 JZVariant.h 的 JZ_DECLARE_METATYPE_INTERNAL),
// 不再需要运行时注册; fromValue / value 直接走内置元类型 id。

static bool s_simRegistered = false;

// ============ 多级 key: 按 '.' 切分 ============
std::vector<std::string> JZSimPack::splitKey(const std::string &key)
{
    std::vector<std::string> parts;
    std::string cur;
    for (char c : key) {
        if (c == '.') { parts.push_back(cur); cur.clear(); }
        else cur += c;
    }
    parts.push_back(cur);
    return parts;
}

// ============ 嵌套递归序列化 (支持 map 中再嵌套 map / list) ============
namespace {
enum { TAG_LEAF = 0, TAG_MAP = 1, TAG_LIST = 2 };
}

void JZSimPack::writeVariant(JZDataStream &s, const JZVariant &v)
{
    int t = v.typeId();
    if (t == jzMetaTypeId<JZVariantMap>()) {
        s << (int32_t)TAG_MAP;
        const JZVariantMap &m = v.valueRef<JZVariantMap>();
        s << (int32_t)m.size();
        for (auto &kv : m) { s << kv.first; writeVariant(s, kv.second); }
    } else if (t == jzMetaTypeId<JZVariantList>()) {
        s << (int32_t)TAG_LIST;
        const JZVariantList &l = v.valueRef<JZVariantList>();
        s << (int32_t)l.size();
        for (auto &e : l) writeVariant(s, e);
    } else {
        s << (int32_t)TAG_LEAF;
        s << v;   // 全局 operator 处理 POD/String/ByteArray
    }
}

JZVariant JZSimPack::readVariant(JZDataStream &s)
{
    int32_t tag; s >> tag;
    if (tag == TAG_MAP) {
        int32_t n; s >> n;
        JZVariantMap m;
        for (int32_t i = 0; i < n; i++) {
            std::string k; s >> k;
            m[k] = readVariant(s);
        }
        return JZVariant::fromValue(m);
    } else if (tag == TAG_LIST) {
        int32_t n; s >> n;
        JZVariantList l;
        l.reserve(n);
        for (int32_t i = 0; i < n; i++)
            l.push_back(readVariant(s));
        return JZVariant::fromValue(l);
    } else {
        JZVariant v; s >> v;
        return v;
    }
}

// 把任意 JZVariant 写入 root 中多级 key 指向的位置(中间节点自动建 map)
void JZSimPack::setField(JZVariantMap &root, const std::string &key, const JZVariant &val)
{
    auto parts = splitKey(key);
    if (parts.empty()) return;
    JZVariantMap *m = &root;
    for (size_t i = 0; i + 1 < parts.size(); i++) {
        auto it = m->find(parts[i]);
        if (it == m->end() || it->second.typeId() != jzMetaTypeId<JZVariantMap>())
            (*m)[parts[i]] = JZVariant::fromValue(JZVariantMap());
        m = &(*m)[parts[i]].valueRef<JZVariantMap>();
    }
    (*m)[parts.back()] = val;
}

// 从 root 中按多级 key 取出 JZVariant
JZResult<JZVariant> JZSimPack::getField(const JZVariantMap &root, const std::string &key)
{
    auto parts = splitKey(key);
    if (parts.empty())
        return JZResult<JZVariant>::Error("empty key");
    const JZVariantMap *m = &root;
    for (size_t i = 0; i + 1 < parts.size(); i++) {
        auto it = m->find(parts[i]);
        if (it == m->end() || it->second.typeId() != jzMetaTypeId<JZVariantMap>())
            return JZResult<JZVariant>::Error("path not found: " + parts[i]);
        m = &it->second.valueRef<JZVariantMap>();
    }
    auto it = m->find(parts.back());
    if (it == m->end())
        return JZResult<JZVariant>::Error("key not found: " + key);
    return JZResult<JZVariant>::Ok(it->second);
}

// ============ 构造 / 注册 ============
JZSimPack::JZSimPack() {}

int JZSimPack::packetType() const { return NetPack_user + 1; }

void JZSimPack::saveToStream(JZDataStream &s) const
{
    s << (int32_t)(int)cmd;
    writeVariant(s, JZVariant::fromValue(data));
}

void JZSimPack::loadFromStream(JZDataStream &s)
{
    int32_t c; s >> c;
    cmd = (JZSimCmd)c;
    JZVariant v = readVariant(s);
    if (v.typeId() == jzMetaTypeId<JZVariantMap>())
        data = v.valueRef<JZVariantMap>();
    else
        data.clear();
}

void JZSimPack::ensureRegistered()
{
    if (s_simRegistered)
        return;
    JZNetPackManager::instance()->registPack<JZSimPack>(JZSimPack::PackType);
    s_simRegistered = true;
}

// ============ 写入 (key 支持多级, 如 "a.b.c") ============
void JZSimPack::setInt(const std::string &key, int v)
{
    setField(data, key, JZVariant(v));
}

void JZSimPack::setInt64(const std::string &key, int64_t v)
{
    setField(data, key, JZVariant(v));
}

void JZSimPack::setBool(const std::string &key, bool v)
{
    setField(data, key, JZVariant(v ? 1 : 0));
}

void JZSimPack::setStr(const std::string &key, const std::string &v)
{
    setField(data, key, JZVariant(v));
}

void JZSimPack::setDouble(const std::string &key, double v)
{
    setField(data, key, JZVariant(v));
}

void JZSimPack::setStrList(const std::string &key, const std::vector<std::string> &v)
{
    JZVariantList l;
    l.reserve(v.size());
    for (auto &e : v) l.push_back(JZVariant(e));
    setField(data, key, JZVariant::fromValue(l));
}

void JZSimPack::setIntList(const std::string &key, const std::vector<int> &v)
{
    JZVariantList l;
    l.reserve(v.size());
    for (int e : v) l.push_back(JZVariant(e));
    setField(data, key, JZVariant::fromValue(l));
}

void JZSimPack::setDoubleList(const std::string &key, const std::vector<double> &v)
{
    JZVariantList l;
    l.reserve(v.size());
    for (double e : v) l.push_back(JZVariant(e));
    setField(data, key, JZVariant::fromValue(l));
}

void JZSimPack::setMap(const std::string &key, const JZVariantMap &v)
{
    setField(data, key, JZVariant::fromValue(v));
}

void JZSimPack::setList(const std::string &key, const JZVariantList &v)
{
    setField(data, key, JZVariant::fromValue(v));
}

// ============ 读取 (统一返回 JZResult, 支持多级 key) ============
JZResult<int> JZSimPack::getInt(const std::string &key) const
{
    auto r = getField(data, key);
    if (r.isError()) return JZResult<int>::Error(r.errorCode(), r.errorMessage());
    const JZVariant &v = r.value();
    int t = v.typeId();
    if (t == JZMetaType::Int)   return JZResult<int>::Ok(v.toInt());
    if (t == JZMetaType::Int64) return JZResult<int>::Ok((int)v.toInt64());
    return JZResult<int>::Error("not an int: " + key);
}

JZResult<int64_t> JZSimPack::getInt64(const std::string &key) const
{
    auto r = getField(data, key);
    if (r.isError()) return JZResult<int64_t>::Error(r.errorCode(), r.errorMessage());
    const JZVariant &v = r.value();
    int t = v.typeId();
    if (t == JZMetaType::Int64) return JZResult<int64_t>::Ok(v.toInt64());
    if (t == JZMetaType::Int)   return JZResult<int64_t>::Ok(v.toInt());
    return JZResult<int64_t>::Error("not an int64: " + key);
}

JZResult<bool> JZSimPack::getBool(const std::string &key) const
{
    auto r = getInt(key);
    if (r.isError()) return JZResult<bool>::Error(r.errorCode(), r.errorMessage());
    return JZResult<bool>::Ok(r.value() != 0);
}

JZResult<std::string> JZSimPack::getStr(const std::string &key) const
{
    auto r = getField(data, key);
    if (r.isError()) return JZResult<std::string>::Error(r.errorCode(), r.errorMessage());
    const JZVariant &v = r.value();
    if (v.typeId() == JZMetaType::String)
        return JZResult<std::string>::Ok(v.toString());
    return JZResult<std::string>::Error("not a string: " + key);
}

JZResult<double> JZSimPack::getDouble(const std::string &key) const
{
    auto r = getField(data, key);
    if (r.isError()) return JZResult<double>::Error(r.errorCode(), r.errorMessage());
    const JZVariant &v = r.value();
    int t = v.typeId();
    if (t == JZMetaType::Double) return JZResult<double>::Ok(v.toDouble());
    if (t == JZMetaType::Float)  return JZResult<double>::Ok(v.toFloat());
    if (t == JZMetaType::Int)    return JZResult<double>::Ok(v.toInt());
    if (t == JZMetaType::Int64)  return JZResult<double>::Ok((double)v.toInt64());
    return JZResult<double>::Error("not a double: " + key);
}

JZResult<std::vector<std::string>> JZSimPack::getStrList(const std::string &key) const
{
    auto r = getField(data, key);
    if (r.isError()) return JZResult<std::vector<std::string>>::Error(r.errorCode(), r.errorMessage());
    const JZVariant &v = r.value();
    if (v.typeId() != jzMetaTypeId<JZVariantList>())
        return JZResult<std::vector<std::string>>::Error("not a list: " + key);
    const JZVariantList &l = v.valueRef<JZVariantList>();
    std::vector<std::string> out;
    out.reserve(l.size());
    for (auto &e : l)
        out.push_back(e.typeId() == JZMetaType::String ? e.toString() : std::string());
    return JZResult<std::vector<std::string>>::Ok(std::move(out));
}

JZResult<std::vector<int>> JZSimPack::getIntList(const std::string &key) const
{
    auto r = getField(data, key);
    if (r.isError()) return JZResult<std::vector<int>>::Error(r.errorCode(), r.errorMessage());
    const JZVariant &v = r.value();
    if (v.typeId() != jzMetaTypeId<JZVariantList>())
        return JZResult<std::vector<int>>::Error("not a list: " + key);
    const JZVariantList &l = v.valueRef<JZVariantList>();
    std::vector<int> out;
    out.reserve(l.size());
    for (auto &e : l) {
        int t = e.typeId();
        if (t == JZMetaType::Int)        out.push_back(e.toInt());
        else if (t == JZMetaType::Int64) out.push_back((int)e.toInt64());
    }
    return JZResult<std::vector<int>>::Ok(std::move(out));
}

JZResult<std::vector<double>> JZSimPack::getDoubleList(const std::string &key) const
{
    auto r = getField(data, key);
    if (r.isError()) return JZResult<std::vector<double>>::Error(r.errorCode(), r.errorMessage());
    const JZVariant &v = r.value();
    if (v.typeId() != jzMetaTypeId<JZVariantList>())
        return JZResult<std::vector<double>>::Error("not a list: " + key);
    const JZVariantList &l = v.valueRef<JZVariantList>();
    std::vector<double> out;
    out.reserve(l.size());
    for (auto &e : l) {
        int t = e.typeId();
        if (t == JZMetaType::Double)      out.push_back(e.toDouble());
        else if (t == JZMetaType::Float)  out.push_back(e.toFloat());
        else if (t == JZMetaType::Int)    out.push_back(e.toInt());
        else if (t == JZMetaType::Int64)  out.push_back((double)e.toInt64());
    }
    return JZResult<std::vector<double>>::Ok(std::move(out));
}

JZResult<JZVariantMap> JZSimPack::getMap(const std::string &key) const
{
    auto r = getField(data, key);
    if (r.isError()) return JZResult<JZVariantMap>::Error(r.errorCode(), r.errorMessage());
    const JZVariant &v = r.value();
    if (v.typeId() != jzMetaTypeId<JZVariantMap>())
        return JZResult<JZVariantMap>::Error("not a map: " + key);
    return JZResult<JZVariantMap>::Ok(v.valueRef<JZVariantMap>());
}

JZResult<JZVariantList> JZSimPack::getList(const std::string &key) const
{
    auto r = getField(data, key);
    if (r.isError()) return JZResult<JZVariantList>::Error(r.errorCode(), r.errorMessage());
    const JZVariant &v = r.value();
    if (v.typeId() != jzMetaTypeId<JZVariantList>())
        return JZResult<JZVariantList>::Error("not a list: " + key);
    return JZResult<JZVariantList>::Ok(v.valueRef<JZVariantList>());
}
