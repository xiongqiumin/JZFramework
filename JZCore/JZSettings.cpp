#include <string>
#include <unordered_map>
#include <fstream>
#include <algorithm>
#include <vector>
#include <utility>
#include "JZSettings.h"
#include "JZStd.h"
#include "JZDebug.h"

// 构造函数
JZSettings::JZSettings()
{
}

// 析构函数
JZSettings::~JZSettings()
{
}

// 打开加载INI文件，标准INI解析：支持;注释、空行、[Section]分组、key=value键值对
bool JZSettings::open(const std::string& iniFilePath)
{
    std::ifstream ifs(iniFilePath, std::ios::in);
    if (!ifs.is_open())
    {
        m_isFileOpen = false;
        m_iniFilePath.clear();
        return false;
    }

    m_iniFilePath = iniFilePath;
    m_isFileOpen = true;
    m_iniData.clear();
    std::string currentSection;
    std::string line;

    int lineNum = 0;  // 行号计数器，在 while 循环之前定义
    while (std::getline(ifs, line))
    {
        lineNum++;  // 每读取一行，行号+1

        // 去除首尾空格和制表符
        line = str_trimmed(line);

        // 跳过空行、跳过;开头的注释行
        if (line.empty() || line[0] == ';')
            continue;

        // 解析INI的[分组/段落]
        if (line.front() == '[' && line.back() == ']')
        {
            currentSection = line.substr(1, line.size() - 2);
            continue;
        }

        // 解析key=value键值对
        size_t equalPos = line.find('=');
        if (equalPos == std::string::npos)
            continue;

        std::string key = line.substr(0, equalPos);
        std::string val = line.substr(equalPos + 1);
        key = str_trimmed(key);
        val = str_trimmed(val);

        if (key.empty())
        {
            jzWarning() << "JZSettings: Empty key found at line " << lineNum << ", skipped: " << line;
            continue;  // 跳过这行，不存储
        }

        // 处理值的配对双引号
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
        {
            // 配对成功，去掉引号
            val = val.substr(1, val.size() - 2);
        }
        else if (val.size() >= 1 && val.front() == '"' && val.back() != '"')
        {
            // 以双引号开头但不是以双引号结尾，引号不配对
            jzWarning() << "JZSettings: Unmatched quote at line " << lineNum << ": " << key << "=" << val
                        << " (keeping original value)";
        }

        // 拼接 分组名.键名 作为内存存储的key，存入容器
        std::string fullKey = currentSection.empty() ? key : currentSection + "." + key;
        if (m_iniData.find(fullKey) != m_iniData.end())
        {
            jzWarning() << "JZSettings: Duplicate key '" << fullKey 
                        << "' at line " << lineNum 
                        << ", overwriting previous value '" << m_iniData[fullKey] << "'";
        }

        m_iniData[fullKey] = std::string(val);
    }

    ifs.close();
    return true;
}

// 开启分组：支持连续调用，多层嵌套分组
bool JZSettings::beginGroup(const std::string& groupName)
{
    if (groupName.empty())
    {
        jzWarning() << "JZSettings::beginGroup: group name cannot be empty";
        return false;
    }

    // 临时栈 = 当前分组栈 + 新分组名
    std::vector<std::string> tempStack = m_groupStack;
    tempStack.push_back(groupName);

    // 压入实际分组栈
    m_groupStack.push_back(groupName);
    std::string cur_group = str_join(m_groupStack, ".");
    std::string cur_group_pre = cur_group + ".";

    // 检查该分组下是否存在任意配置项
    bool groupExist = false;
    for (const auto& pair : m_iniData)
    {
        const std::string& fullKey = pair.first;
        if (str_startsWith(fullKey, cur_group_pre))
        {
            groupExist = true;
            break;
        }
    }

    if (!groupExist)
    {
        jzWarning() << "JZSettings::beginGroup: group [" << cur_group << "] not exists in ini data";
    }

    return groupExist;
}

// 结束分组：出栈当前分组，回到上一级，栈空则无操作
void JZSettings::endGroup()
{
    if (!m_groupStack.empty())
    {
        m_groupStack.pop_back();
    }
}

bool JZSettings::hasGroup(const std::string& groupName)
{
    std::string cur_group = str_join(m_groupStack, ".") + groupName;
    std::string cur_group_pre = cur_group + ".";

    // 检查该分组下是否存在任意配置项
    bool groupExist = false;
    for (const auto& pair : m_iniData)
    {
        const std::string& fullKey = pair.first;
        if (str_startsWith(fullKey, cur_group_pre))
        {
            groupExist = true;
            break;
        }
    }
    return groupExist;
}

// 私有方法：拼接当前所有嵌套分组 + 传入的key，生成完整键名
std::string JZSettings::getFullKey(const std::string& key)
{
    std::string fullKey;
    for (const auto& group : m_groupStack)
    {
        if (!fullKey.empty())
            fullKey += ".";
        fullKey += group;
    }

    if (!fullKey.empty() && !key.empty())
        fullKey += ".";
    fullKey += key;

    return fullKey;
}

// 私有方法：拆分完整键名，比如 "Database.Mysql.Port" -> pair("Database.Mysql", "Port")
std::pair<std::string, std::string> JZSettings::splitGroupAndKey(const std::string& fullKey)
{
    size_t lastDotPos = fullKey.find_last_of('.');
    if (lastDotPos == std::string::npos)
    {
        // 无分组的全局键值对
        return { "", fullKey };
    }
    std::string group = fullKey.substr(0, lastDotPos);
    std::string key = fullKey.substr(lastDotPos + 1);
    return { group, key };
}

// 设置值：自动拼接当前分组，存在则覆盖，不存在则新增，文件未打开则无操作
void JZSettings::setValue(std::string name, std::string v)
{
    if (!m_isFileOpen || name.empty())
        return;

    std::string fullKey = getFullKey(name);
    m_iniData[std::move(fullKey)] = std::move(v);
}

bool JZSettings::hasValue(std::string name)
{
    std::string fullKey = getFullKey(name);
    auto iter = m_iniData.find(fullKey);
    if (iter != m_iniData.end())
        return true;
    else
        return false;
}

// 获取值：自动拼接当前分组，找到返回对应值，找不到返回默认值
std::string JZSettings::getValue(std::string name, const std::string& default_value)
{
    if (!m_isFileOpen || name.empty())
        return default_value;

    std::string fullKey = getFullKey(name);
    auto iter = m_iniData.find(fullKey);
    if (iter != m_iniData.end())
    {
        return iter->second;
    }
    return default_value;
}

void JZSettings::setBool(const std::string& name, bool value) {
    setValue(name, value ? "true" : "false");
}

bool JZSettings::getBool(const std::string& name, bool default_value) {
    std::string s = getValue(name);
    if (s.empty()) 
        return default_value;
    
    bool ok = false;
    bool v = false;
    if(s == "1" || s == "true")
    {
        v = true;
        ok = true;
    }
    else if(s == "0" || s == "false")
    {
        v = false;
        ok = true;
    }
    else
    {
        ok = false;
        jzWarning() << "JZSettings::getBool(\"" << name << "\") failed: value \"" << s
                    << "\" is not a valid bool (expected true/false, 1/0)";
    }
    return ok ? v : default_value;
}

void JZSettings::setInt(const std::string& name, int value) {
    setValue(name, std::to_string(value));
}

int JZSettings::getInt(const std::string& name, int default_value) {
    std::string s = getValue(name);
    if (s.empty()) return default_value;
    bool ok = false;
    int v = str_toInt(s, &ok);
    if (!ok)
    {
        jzWarning() << "JZSettings::getInt(\"" << name << "\") failed: value \"" << s
                    << "\" is not a valid integer, using default: " << default_value;
    }
    return ok ? v : default_value;
}


void JZSettings::setDouble(const std::string& name, double value) {
    setValue(name, std::to_string(value));
}

double JZSettings::getDouble(const std::string& name, double default_value) {
    std::string s = getValue(name);
    if (s.empty()) return default_value;
    bool ok = false;
    double v = str_toDouble(s, &ok);
    if (!ok)
    {
        jzWarning() << "JZSettings::getDouble(\"" << name << "\") failed: value \"" << s
                    << "\" is not a valid double, using default: " << default_value;
    }
    return ok ? v : default_value;
}

// ===================== 【已修复 核心重点】 sync() 标准INI文件写入 =====================
void JZSettings::sync()
{
    // 文件未打开/路径为空，直接返回
    if (!m_isFileOpen || m_iniFilePath.empty())
        return;

    // 清空文件内容，重新写入标准INI格式
    std::ofstream ofs(m_iniFilePath, std::ios::out | std::ios::trunc);
    if (!ofs.is_open())
        return;

    std::string currentWriteGroup;
    // 遍历所有内存中的配置项，写入标准INI格式
    for (const auto& pair : m_iniData)
    {
        const std::string& fullKey = pair.first;
        const std::string& value = pair.second;
        // 拆分分组名和键名
        std::pair<std::string, std::string> group_pair = splitGroupAndKey(fullKey);
        auto group = group_pair.first;
        auto key = group_pair.second;

        // 分组变更时，写入新的 [分组名] 段落头
        if (group != currentWriteGroup)
        {
            currentWriteGroup = group;
            if (!group.empty())
            {
                ofs << "\n[" << group << "]\n";
            }
        }
        // 写入 键=值 标准格式
        ofs << key << "=" << value << "\n";
    }

    ofs.flush(); // 强制刷入磁盘
    ofs.close();
}