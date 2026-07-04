#ifndef STD_HELPER_H_
#define STD_HELPER_H_


#include <algorithm>
#include <string>
#include <vector>
#include <map>
#include <assert.h>
#include "JZGlobal.h"

//std::string
std::string str_asprintf(const char *fmt, ...);
std::string str_vasprintf(const char *fmt, va_list ap);

bool str_startsWith(const std::string &str, const std::string &prefix, bool caseSensitive = true);
bool str_endsWith(const std::string &str, const std::string &suffix, bool caseSensitive = true);
bool str_contains(const std::string &str, const std::string &substr, bool caseSensitive = true);
void str_chop(std::string &str, int n);
std::string str_mid(const std::string &str, size_t pos, int len = -1);
std::string str_left(const std::string& s, size_t n);
std::string str_right(const std::string& s, size_t n);
void str_replace(std::string &str, const std::string &oldstr, const std::string &newstr);
void str_remove(std::string &str, const std::string &substr);
void str_prepend(std::string& src, const std::string& prefix);
void str_append(std::string& str, const std::string& other);
std::string str_toLower(const std::string &str);
std::string str_toUpper(const std::string &str);
std::string str_trimmed(const std::string &str);
std::string str_normalized(const std::string &str);
int str_indexOf(const std::string &str, const std::string &substr, int pos = 0, bool caseSensitive = true);
int str_lastIndexOf(const std::string &str, const std::string &substr, int pos = -1, bool caseSensitive = true);
std::vector<std::string> str_split(const std::string &str, const std::string &sep, JZ::SplitBehavior behavior = JZ::KeepEmptyParts);
std::string str_join(const std::vector<std::string> &str_list, const std::string &sep);

int str_toInt(const std::string& str, bool* ok = nullptr,int base = 10);
double str_toDouble(const std::string& str, bool* ok = nullptr);

std::string str_local_to_utf8(const std::string& local);
std::string str_utf8_to_local(const std::string& local);
std::wstring str_to_wstr(const std::string& str);
std::string wstr_to_str(const std::wstring& str);

std::string str_escape(const std::string& src);
std::string str_unescape(const std::string& src);

//vector
template<typename T>
int vec_indexOf(const std::vector<T>& vec, const T& value)
{
    auto it = std::find(vec.begin(), vec.end(), value);
    if (it == vec.end())
        return -1;

    return (int)(it - vec.begin());
}

template<typename T>
void vec_popFront(std::vector<T>& vec)
{
    vec.erase(vec.begin() + 0);
}

template<typename T>
void vec_removeAt(std::vector<T>& vec, int index)
{    
    vec.erase(vec.begin() + index);
}

template<typename T>
void vec_removeOne(std::vector<T>& vec, const T& value)
{
    auto it = std::find(vec.begin(), vec.end(), value);
    if (it != vec.end())
        vec.erase(it);
}

template<typename T>
void vec_removeAll(std::vector<T>& vec, const T& value)
{
    auto last = std::remove(vec.begin(), vec.end(), value);
    vec.erase(last, vec.end());
}

template<typename T>
void vec_append(std::vector<T>& dst, const std::vector<T>& src)
{
    dst.insert(dst.end(), src.begin(), src.end());
}

template<typename T>
std::vector<T> vec_mid(const std::vector<T>& vec, int start, int count = -1)
{
    int n = static_cast<int>(vec.size());
    JZ_ASSERT(start >= 0 && start < n && (count >= 0 || count == -1));

    // 计算结束位置
    int end;
    if (count < 0)
        end = n;
    else
        end = start + count;

    if (end > n) 
        end = n;

    return std::vector<T>(vec.begin() + start, vec.begin() + end);
}

template<typename T>
bool vec_contains(const std::vector<T>& vec, const T& value)
{
    return vec_indexOf(vec,value) != -1;
}

//map
template<typename K, typename V>
void map_insert(std::map<K, V>& map, const K& key, const V& value)
{
    auto it = map.find(key);
    JZ_ASSERT(it == map.end());
    map[key] = value;
}

template<typename K, typename V>
void map_remove(std::map<K, V>& map, const K& key)
{
    auto it = map.find(key);
    if(it != map.end())
        map.erase(it);
}

template<typename K, typename V>
bool map_contains(const std::map<K, V>& map, const K& key)
{
    return map.find(key) != map.end();
}

template<typename K, typename V>
V map_value(const std::map<K, V>& map, const K& key, const V& defaultValue = V())
{
    auto it = map.find(key);
    if (it != map.end())
        return it->second;
    else
        return defaultValue;
}

template<typename K, typename V>
std::vector<V> map_values(const std::map<K, V>& map)
{
    std::vector<V> list;
    for (const auto& pair : map)
    {
        list.push_back(pair.second);
    }
    return list;
}

// 获取值对应的第一个键（不存在返回默认键）
template<typename K, typename V>
K map_key(const std::map<K, V>& map, const V& value, const K& defaultKey = K())
{
    for (const auto& pair : map)
    {
        if (pair.second == value)
            return pair.first;
    }
    return defaultKey;
}

template <typename K, typename V>
std::vector<K> map_keys(const std::map<K, V>& map)
{
    std::vector<K> list;
    for (const auto& pair : map)
    {
        list.push_back(pair.first);
    }
    return list;
}

template <typename T>
inline void jzDeleteAll(const std::vector<T> &c)
{
    auto begin = c.begin();
    auto end = c.end();
    while (begin != end) {
        delete *begin;
        ++begin;
    }
}

template <typename K, typename V>
inline void jzDeleteAll(const std::map<K, V> &c)
{
    auto begin = c.begin();
    auto end = c.end();
    while (begin != end) {
        delete begin->second;
        ++begin;
    }
}

#endif // !
