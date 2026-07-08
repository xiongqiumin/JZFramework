#include "JZResources.h"
#include "JZStd.h"

JZResources *JZResources::instance()
{    
    static JZResources inst;
    return &inst;
}

void JZResources::add(const std::string &name, const unsigned char *data, int size)
{
    m_resources[name] = {data, size};
}

const JZResourceData *JZResources::find(const std::string &name) const
{
    auto it = m_resources.find(name);
    return it != m_resources.end() ? &it->second : nullptr;
}

std::vector<std::string> JZResources::resourcesList() const
{
    return map_keys(m_resources);
}