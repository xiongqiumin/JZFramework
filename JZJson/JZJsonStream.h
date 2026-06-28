#ifndef JSON_STREAM_H_
#define JSON_STREAM_H_

#include "JZStd.h"
#include "JZString.h"
#include "JZOptional.h"
#include "json/json.h"

class JZJsonStream
{
public:
    JZJsonStream();
    explicit JZJsonStream(Json::Value* value);
    explicit JZJsonStream(const Json::Value* value);
    ~JZJsonStream();

    bool isError();
    void clearError();
    void setErrorString(std::string error);
    std::string errorString();

    void open(Json::Value* value);
    void open(const Json::Value* value);
    void close();

    Json::Value* curValue();
    void push(std::string name, Json::Value* value);
    void pop();

    void operator<<(const bool& t);
    void operator>>(bool& t);

    void operator<<(const int& t);
    void operator>>(int& t);

    void operator<<(const int64_t& t);
    void operator>>(int64_t& t);

    void operator<<(const float& t);
    void operator>>(float& t);

    void operator<<(const double& t);
    void operator>>(double& t);

    void operator<<(const std::string& t);
    void operator>>(std::string& t);

    void operator<<(const JZString& t);
    void operator>>(JZString& t);

    template <class T>
    bool readMember(const std::string& name, T& t)
    {
        if (isError())
            return false;

        int pre_level = m_values.size();
        if (!switchToMember(name))
            return false;

        *this >> t;
        popTo(pre_level);

        return !isError();
    }

    template <class T>
    bool readMemberDefault(const std::string& name, T& t, const T& default_t)
    {
        if (isError())
            return false;

        int pre_level = m_values.size();
        int last_idx = str_lastIndexOf(name, ".");
        string child_name;
        if (last_idx == -1)
        {
            child_name = name;
        }
        else
        {
            string parent_name = str_mid(name, 0, last_idx);
            if (!switchToMember(parent_name))
                return false;

            child_name = str_mid(name, last_idx + 1);
        }

        Json::Value* value = curValue();
        if (value->type() != Json::objectValue)
        {
            m_errorString = "is not object";
            popTo(pre_level);
            return false;
        }

        if (!value->isMember(child_name))
        {
            t = default_t;
            popTo(pre_level);
            return true;
        }

        push(child_name, &(*value)[child_name]);
        *this >> t;
        popTo(pre_level);

        return !isError();
    }

    template <class T>
    bool readMemberOptional(const std::string& name, JZOptional<T>& t)
    {
        if (isError())
            return false;

        int pre_level = m_values.size();
        int last_idx = str_lastIndexOf(name, ".");
        string child_name;
        if (last_idx == -1)
        {
            child_name = name;
        }
        else
        {
            string parent_name = str_mid(name, 0, last_idx);
            if (!switchToMember(parent_name))
                return false;

            child_name = str_mid(name, last_idx + 1);
        }

        Json::Value* value = curValue();
        if (value->type() != Json::objectValue)
        {
            m_errorString = "is not object";
            popTo(pre_level);
            return false;
        }

        if (!value->isMember(child_name))
        {
            t = JZOptional<T>::None();
            popTo(pre_level);
            return true;
        }

        push(child_name, &(*value)[child_name]);
        T tmp;
        *this >> tmp;
        if(!isError())
            t = tmp;
        popTo(pre_level);

        return !isError();
    }

    template <class T>
    bool writeMember(const std::string& name, const T& t)
    {
        Json::Value* value = curValue();
        if (value->type() != Json::nullValue && value->type() != Json::objectValue)
        {
            m_errorString = "is not object";
            return false;
        }

        if (value->type() != Json::objectValue || value->isMember(name))
        {
            m_errorString = name + " is exist";
            return false;
        }

        push(name, &(*value)[name]);
        *this << t;
        pop();
        return true;
    }

protected:
    struct JsonPath
    {
        std::string name;
        Json::Value* value;
    };

    bool switchToMember(const std::string& name);
    void popTo(int level);

    void makeError(std::string str);
    std::string m_errorString;
    std::vector<JsonPath> m_values;
};

template <class T>
void operator<<(JZJsonStream& s, const std::vector<T>& list)
{
    Json::Value* value = s.curValue();
    Json::Value array(Json::arrayValue);
    for (int i = 0; i < (int)list.size(); i++)
    {
        Json::Value v;
        s.push("[" + to_string(i) + "]", &v);
        s << list[i];
        s.pop();

        array.append(v);
    }

    *value = array;
}

template <class T>
void operator>>(JZJsonStream& s, std::vector<T>& list)
{
    Json::Value* value = s.curValue();
    if (value->type() != Json::arrayValue)
    {
        s.setErrorString("value is not array");
        return;
    }

    for (int i = 0; i < (int)value->size(); i++)
    {
        T t;

        s.push("[" + to_string(i) + "]", &(*value)[i]);
        s >> t;
        s.pop();

        list.push_back(t);
    }
}

template <class T>
void operator<<(JZJsonStream& s, const std::map<std::string, T>& mapData)
{
    Json::Value* value = s.curValue();
    Json::Value obj(Json::objectValue);

    for (const auto& pair : mapData)
    {
        const std::string& key = pair.first;
        const T& data = pair.second;

        Json::Value v;
        s.push(key, &v);
        s << data;
        s.pop();

        obj[key] = v;
    }

    *value = obj;
}

template <class T>
void operator>>(JZJsonStream& s, std::map<std::string, T>& mapData)
{
    Json::Value* value = s.curValue();
    if (value->type() != Json::objectValue)
    {
        s.setErrorString("value is not object");
        return;
    }

    mapData.clear();
    const Json::Value::Members& keys = value->getMemberNames();
    for (const std::string& key : keys)
    {
        T t;
        s.push(key, &(*value)[key]);
        s >> t;
        s.pop();

        mapData.emplace(key, std::move(t));
    }
}

class JsonObjectReader
{
public:
    // 构造函数：检查当前值是否为对象
    explicit JsonObjectReader(JZJsonStream& json_stream)
        : s(json_stream)
    {
        if (!s.curValue()->isObject())
        {
            s.setErrorString("is not object");
        }
        else
        {
            current_member_list = s.curValue()->getMemberNames();
        }
    }

    virtual ~JsonObjectReader() = default;

    // 强制读取成员，若成员不存在或类型不匹配则设置错误
    template <class T>
    bool read(const std::string& name, T& ptr)
    {
        if (s.isError())
            return false;

        read_member_list.push_back(name);
        return s.readMember(name, ptr);
    }

    // 可选读取成员：若成员存在则读取并覆盖 ptr，否则保持 ptr 原值（不设置错误）
    template <class T>
    bool readOptional(const std::string& name, JZOptional<T> &ptr)
    {
        if (s.isError())
            return false;
        
        read_member_list.push_back(name);
        return s.readMemberOptional(name, ptr);
    }

    // 可选读取成员：若成员存在则读取并覆盖 ptr，否则保持 ptr 原值（不设置错误）
    template <class T>
    bool readDefault(const std::string& name, T& ptr, const T &value = T())
    {
        if (s.isError())
            return false;
        
        read_member_list.push_back(name);
        return s.readMemberDefault(name, ptr, value);
    }

    // 检查是否有未知成员（已读取列表之外的成员）
    bool checkUnknownMember()
    {
        if (s.isError())
            return false;
        
        for (const auto& member : current_member_list)
        {
            if (std::find(read_member_list.begin(), read_member_list.end(), member) == read_member_list.end())
            {
                s.setErrorString("unknown member " + member);
                return false;
            }
        }
        return true;
    }

private:
    JZJsonStream& s;                               // 使用引用，避免拷贝
    std::vector<std::string> current_member_list;
    std::vector<std::string> read_member_list;   // 记录已读取的成员名
};

class JZJsonHelper
{
public:
    template <class T>
    static bool loadFromJson(T& t, const Json::Value& obj, std::string& error_message)
    {
        JZJsonStream s;
        s.open((Json::Value*)&obj);
        s >> t;
        if (s.isError())
        {
            error_message = s.errorString();
            return false;
        }
        return true;
    }

    template <class T>
    static void saveToJson(const T& t, Json::Value& obj)
    {
        JZJsonStream s;
        s.open(&obj);
        s << t;
    }

    static bool isRpcSuccess(const Json::Value& obj, std::string& error);
};

#endif  // !JSON_STREAM_H_
