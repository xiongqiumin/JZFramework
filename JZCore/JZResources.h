#ifndef JZ_RESOURCES_H_
#define JZ_RESOURCES_H_

#include <string>
#include <map>
#include "JZStd.h"

struct JZResourceData
{
    const unsigned char *data;
    int size;
};

class JZResources
{
public:
    static JZResources *instance();

    void add(const std::string &name, const unsigned char *data, int size);
    const JZResourceData *find(const std::string &name) const;

private:
    JZResources() = default;
    JZ_DISABLE_COPY(JZResources)

    std::map<std::string, JZResourceData> m_resources;
    static JZResources *s_instance;
};

#endif // JZ_RESOURCES_H_
