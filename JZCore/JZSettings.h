#ifndef JZ_SETTINGS_H_
#define JZ_SETTINGS_H_

#include <map>
#include <vector>
#include <string>
#include "JZGlobal.h"

class JZSettings
{
public:
    JZSettings();
    ~JZSettings();

    // 打开并加载指定的INI配置文件，成功true，失败false
    bool open(const std::string& iniFilePath);
    void sync(); // 已修复：写入标准格式的INI文件

    // 分组操作 - 对应INI的[GroupName]段落
    bool beginGroup(const std::string& groupName);
    void endGroup();
    bool hasGroup(const std::string& groupName);

    bool hasValue(std::string name);
    void setValue(std::string name, std::string v);
    std::string getValue(std::string name, const std::string& default_value = std::string());

    void setBool(const std::string& name, bool value);
    bool getBool(const std::string& name, bool default_value = false);
    
    void setInt(const std::string& name, int value);
    int getInt(const std::string& name, int default_value = 0);

    void setDouble(const std::string& name, double value);
    double getDouble(const std::string& name, double default_value = 0.0);

private:
    // 私有核心方法
    std::string getFullKey(const std::string& key);
    // 拆分完整键名 分组.键名 -> pair<分组名, 键名>
    std::pair<std::string, std::string> splitGroupAndKey(const std::string& fullKey);

private:
    std::map<std::string, std::string> m_iniData; // INI键值对核心存储
    std::string m_iniFilePath;                            // 当前打开的INI文件路径
    bool m_isFileOpen = false;                            // INI文件是否成功打开
    std::vector<std::string> m_groupStack;                // 分组栈，支持多层嵌套分组
};


#endif // ! JZ_SETTINGS_H_