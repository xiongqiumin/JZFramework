#include "JZJsonStream.h"

#include "JZStd.h"

JZJsonStream::JZJsonStream()
{
}

JZJsonStream::JZJsonStream(Json::Value* value)
{
    open(value);
}

JZJsonStream::JZJsonStream(const Json::Value* value)
{
    open(value);
}

JZJsonStream::~JZJsonStream()
{
}

void JZJsonStream::open(Json::Value* value)
{
    JZ_ASSERT(m_values.empty());
    push("", value);
}

void JZJsonStream::open(const Json::Value* value)
{
    JZ_ASSERT(m_values.empty());
    push("", (Json::Value*)value);
}

void JZJsonStream::close()
{
    m_values.clear();
}

void JZJsonStream::popTo(int level)
{
    while (m_values.size() > level)
        m_values.pop_back();
}

bool JZJsonStream::switchToMember(const std::string& full_name)
{
    int pre_level = (int)m_values.size();

    vector<string> name_list = str_split(full_name, ".");
    Json::Value* value = curValue();
    for (int i = 0; i < name_list.size(); i++)
    {
        string name = name_list[i];
        int array_idx = -1;

        string base_name;
        if (!str_endsWith(name, "]"))
        {
            base_name = name;
        }
        else
        {
            int s = str_lastIndexOf(name, "[");
            if (s == string::npos)
            {
                makeError("invalid array syntax: missing '['");
                popTo(pre_level);
                return false;
            }
            base_name = name.substr(0, s);

            s++;
            string str_idx = name.substr(s, name.size() - s - 1);
            bool ok;
            array_idx = str_toInt(str_idx, &ok);
            if (!ok || array_idx < 0)
            {
                makeError(str_idx + " is not a valid number");
                popTo(pre_level);
                return false;
            }
        }

        if (value->type() != Json::objectValue)
        {
            makeError("is not an object");
            popTo(pre_level);
            return false;
        }

        if (!value->isMember(base_name))
        {
            makeError("missing member: " + base_name);
            popTo(pre_level);
            return false;
        }

        value = &(*value)[base_name];
        if (array_idx == -1)
        {
            push(name, value);
        }
        else
        {
            if (value->type() != Json::arrayValue)
            {
                makeError("node is not an array");
                popTo(pre_level);
                return false;
            }

            if (array_idx < 0 || array_idx >= (int)value->size())
            {
                makeError("array index " + to_string(array_idx) +
                          " out of range (size: " + std::to_string(value->size()) + ")");
                popTo(pre_level);
                return false;
            }

            value = &(*value)[array_idx];
            push(name, value);
        }
    }

    return true;
}

void JZJsonStream::push(std::string name, Json::Value* value)
{
    m_values.push_back({name, value});
}

void JZJsonStream::pop()
{
    m_values.pop_back();
}

void JZJsonStream::makeError(std::string str)
{
    string pre;
    for (int i = 1; i < m_values.size(); i++)
    {
        if (!pre.empty())
            pre += ".";
        pre += m_values[i].name;
    }
    if (!pre.empty())
        pre += " ";

    m_errorString = pre + str;
}

Json::Value* JZJsonStream::curValue()
{
    return m_values.back().value;
}

bool JZJsonStream::isError()
{
    return !m_errorString.empty();
}

void JZJsonStream::clearError()
{
    m_errorString.clear();
}

void JZJsonStream::setErrorString(std::string error)
{
    makeError(error);
}

std::string JZJsonStream::errorString()
{
    return m_errorString;
}

void JZJsonStream::operator<<(const bool& t)
{
    auto* value = curValue();
    *value = t;
}

void JZJsonStream::operator>>(bool& t)
{
    auto* value = curValue();
    if (value->type() != Json::booleanValue)
    {
        makeError("value not bool");
        return;
    }
    t = value->asBool();
}

void JZJsonStream::operator<<(const int& t)
{
    auto* value = curValue();
    *value = t;
}

void JZJsonStream::operator>>(int& t)
{
    auto* value = curValue();
    if (value->type() != Json::intValue)
    {
        makeError("value not int");
        return;
    }
    t = value->asInt();
}

void JZJsonStream::operator<<(const int64_t& t)
{
    auto* value = curValue();
    *value = Json::Int64(t);
}

void JZJsonStream::operator>>(int64_t& t)
{
    auto* value = curValue();
    if (value->type() != Json::intValue)
    {
        makeError("value not int");
        return;
    }
    t = value->asInt64();
}

void JZJsonStream::operator<<(const float& t)
{
    auto* value = curValue();
    *value = (double)t;
}

void JZJsonStream::operator>>(float& t)
{
    auto* value = curValue();
    if (value->type() != Json::realValue && value->type() != Json::intValue)
    {
        makeError("value not float");
        return;
    }
    t = value->asFloat();
}

void JZJsonStream::operator<<(const double& t)
{
    auto* value = curValue();
    *value = t;
}

void JZJsonStream::operator>>(double& t)
{
    auto* value = curValue();
    if (value->type() != Json::realValue && value->type() != Json::intValue)
    {
        makeError("value not float");
        return;
    }
    t = value->asDouble();
}

void JZJsonStream::operator<<(const std::string& t)
{
    auto* value = curValue();
    *value = t;
}

void JZJsonStream::operator>>(std::string& t)
{
    auto* value = curValue();
    if (value->type() != Json::stringValue)
    {
        makeError("value not string");
        return;
    }
    t = value->asString();
}

void JZJsonStream::operator<<(const JZString& t)
{
    auto* value = curValue();
    *value = t.toStdString();
}

void JZJsonStream::operator>>(JZString& t)
{
    auto* value = curValue();
    if (value->type() != Json::stringValue)
    {
        makeError("value not string");
        return;
    }
    t = JZString::fromStdString(value->asString());
}

// JZJsonHelper
bool JZJsonHelper::isRpcSuccess(const Json::Value& res, std::string& errMsg)
{
    if (res.isMember("error") && !res["error"].isNull())
    {
        if (res["error"].isObject())
        {
            errMsg = res["error"]["message"].asString();
        }
        else if (res["error"].isString())
        {
            errMsg = res["error"].asString();
        }
        else
        {
            errMsg = "unknown error";
        }
        return false;
    }
    else
    {
        return true;
    }
}