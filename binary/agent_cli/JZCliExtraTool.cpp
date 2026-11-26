#include "JZCliExtraTool.h"

#include "JZAgentTool.h"
#include "JZMutex.h"

#include <map>

namespace {

struct Registry
{
    JZMutex mutex;
    std::map<std::string, JZCliExtraToolFactory> factories;
};

Registry &registry()
{
    static Registry r;
    return r;
}

} // namespace

void jzRegisterCliExtraTool(const std::string &name, const JZCliExtraToolFactory &factory)
{
    if (name.empty() || !factory)
        return;
    Registry &r = registry();
    JZMutexLocker lock(&r.mutex);
    r.factories[name] = factory;
}

void jzUnregisterCliExtraTool(const std::string &name)
{
    Registry &r = registry();
    JZMutexLocker lock(&r.mutex);
    r.factories.erase(name);
}

JZAgentTool *jzCreateCliExtraTool(const std::string &name, JZObject *parent)
{
    Registry &r = registry();
    JZCliExtraToolFactory factory;
    {
        JZMutexLocker lock(&r.mutex);
        auto it = r.factories.find(name);
        if (it == r.factories.end())
            return nullptr;
        factory = it->second;
    }
    return factory(parent);
}
