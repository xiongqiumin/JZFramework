#include "JZPlugin.h"

#include <string>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

// 插件动态库必须导出的 C 符号名。
static const char* kJZPluginCreateSymbol  = "JZPluginCreate";
static const char* kJZPluginDestroySymbol = "JZPluginDestroy";

JZPluginManager::JZPluginManager()
{
}

JZPluginManager::~JZPluginManager()
{
    unloadAll();
}

void JZPluginManager::closeEntry(PluginEntry& entry)
{
    if (entry.plugin && entry.destroy)
        entry.destroy(entry.plugin);
    entry.plugin = nullptr;
    entry.destroy = nullptr;

    if (entry.handle)
    {
#if defined(_WIN32)
        FreeLibrary((HMODULE)entry.handle);
#else
        dlclose(entry.handle);
#endif
        entry.handle = nullptr;
    }
}

JZPlugin* JZPluginManager::load(const std::string& path)
{
    m_lastError.clear();

#if defined(_WIN32)
    HMODULE handle = LoadLibraryA(path.c_str());
    if (!handle)
    {
        m_lastError = "LoadLibrary failed: " + path;
        return nullptr;
    }
    auto create  = (JZPluginCreateFunc)GetProcAddress(handle, kJZPluginCreateSymbol);
    auto destroy = (JZPluginDestroyFunc)GetProcAddress(handle, kJZPluginDestroySymbol);
#else
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle)
    {
        const char* err = dlerror();
        m_lastError = std::string("dlopen failed: ") + (err ? err : path.c_str());
        return nullptr;
    }
    auto create  = (JZPluginCreateFunc)dlsym(handle, kJZPluginCreateSymbol);
    auto destroy = (JZPluginDestroyFunc)dlsym(handle, kJZPluginDestroySymbol);
#endif

    if (!create || !destroy)
    {
        m_lastError = "plugin entrypoint missing: " + std::string(kJZPluginCreateSymbol)
                    + " / " + std::string(kJZPluginDestroySymbol);
#if defined(_WIN32)
        FreeLibrary((HMODULE)handle);
#else
        dlclose(handle);
#endif
        return nullptr;
    }

    JZPlugin* plugin = create();
    if (!plugin)
    {
        m_lastError = "JZPluginCreate returned null";
#if defined(_WIN32)
        FreeLibrary((HMODULE)handle);
#else
        dlclose(handle);
#endif
        return nullptr;
    }

    if (!plugin->init())
    {
        m_lastError = "plugin init failed: " + plugin->name();
        destroy(plugin);
#if defined(_WIN32)
        FreeLibrary((HMODULE)handle);
#else
        dlclose(handle);
#endif
        return nullptr;
    }

    PluginEntry entry;
    entry.plugin  = plugin;
    entry.handle  = handle;
    entry.destroy = destroy;
    m_entries.push_back(entry);
    m_plugins.push_back(plugin);
    return plugin;
}

void JZPluginManager::unload(JZPlugin* plugin)
{
    for (size_t i = 0; i < m_entries.size(); ++i)
    {
        if (m_entries[i].plugin != plugin)
            continue;

        if (plugin)
            plugin->uninit();
        closeEntry(m_entries[i]);

        m_entries.erase(m_entries.begin() + i);
        // 同步 m_plugins 列表
        for (size_t j = 0; j < m_plugins.size(); ++j)
        {
            if (m_plugins[j] == plugin)
            {
                m_plugins.erase(m_plugins.begin() + j);
                break;
            }
        }
        return;
    }
}

void JZPluginManager::unloadAll()
{
    for (PluginEntry& entry : m_entries)
    {
        if (entry.plugin)
            entry.plugin->uninit();
        closeEntry(entry);
    }
    m_entries.clear();
    m_plugins.clear();
}
