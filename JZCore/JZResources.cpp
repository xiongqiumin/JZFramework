#include "JZResources.h"

JZResources *JZResources::s_instance = nullptr;

JZResources *JZResources::instance()
{
    if (!s_instance)
        s_instance = new JZResources();
    return s_instance;
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
