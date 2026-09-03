#include "JZResources.h"
#include "JZStd.h"
#include "JZDebug.h"

// 从资源名 ":/xxx/yyy" 中解析出资源组名 xxx (对应注册函数 jzres_xxx_init())
static std::string jzResourceGroup(const std::string &name)
{
    size_t start = 0;
    if (start < name.size() && name[start] == ':') start++;
    if (start < name.size() && name[start] == '/') start++;
    size_t end = name.find('/', start);
    if (end == std::string::npos) end = name.size();
    return name.substr(start, end - start);
}

JZResources *JZResources::instance()
{    
    static JZResources inst;
    return &inst;
}

void JZResources::add(const std::string &name, const unsigned char *data, int size, int64_t mtime)
{
    JZResourceData res_data;
    res_data.data = data;
    res_data.size = size;
    res_data.mtime = mtime;
    m_resources[name] = res_data;
}

const JZResourceData *JZResources::find(const std::string &name) const
{
    auto it = m_resources.find(name);
    if(it == m_resources.end())
    {
        // 资源名由 jz_resources.py 生成, 格式固定为 ":/xxx/yyy" (以 ':' 开头),
        // 其中 xxx 为资源组名, 对应注册函数 jzres_xxx_init()。
        std::string group = jzResourceGroup(name);
        if(group.empty())
        {
            jzWarning() << "JZResources::find: invalid res" << name;
        }
        else
        {
            std::string init_func = "jzres_" + group + "_init()";
            jzWarning() << "JZResources::find: resource \"" << name << "\" faild, "
                        << "please check is" << init_func.c_str() << "invoke";
        }

        return nullptr;
    } 
    return &it->second;
}

bool JZResources::contains(const std::string &name) const
{
    return m_resources.find(name) != m_resources.end();
}

std::vector<std::string> JZResources::resourcesList() const
{
    return map_keys(m_resources);
}