#ifndef JZ_PLUGIN_H_
#define JZ_PLUGIN_H_

#include "JZGlobal.h"
#include <string>
#include <vector>

// ============================================================================
// JZPlugin / JZPluginManager — 运行时插件机制
//
// 仅在把 JZFramework 编译成**动态库**(-DJZFRAMEWORK_DYNAMIC / SHARED)时参与编译,
// 见 JZCore.cmake。静态库场景没有"运行时加载另一个 .dll/.so"的意义, 故跳过。
//
// 一个插件 = 一个独立的动态库, 至少导出两个 C 符号:
//   extern "C" JZPlugin* JZPluginCreate();              // 构造插件实例
//   extern "C" void      JZPluginDestroy(JZPlugin*);    // 销毁插件实例
// ============================================================================

// 插件基类: 各插件动态库继承并实现。
class JZPlugin
{
public:
    virtual ~JZPlugin() {}

    // 插件名称(用于调试/去重)。
    virtual std::string name() const = 0;

    // 加载后调用: 失败返回 false, 管理器会立即卸载该插件。
    virtual bool init() { return true; }

    // 卸载前调用: 释放插件内部资源。
    virtual void uninit() {}
};

// 插件动态库必须实现的 C 接口签名。
typedef JZPlugin* (*JZPluginCreateFunc)();
typedef void     (*JZPluginDestroyFunc)(JZPlugin*);

// 插件管理器: 运行时加载 / 卸载插件动态库。
class JZFRAMEWORK_API JZPluginManager
{
public:
    JZPluginManager();
    ~JZPluginManager();

    // 加载插件动态库(绝对或相对路径)。
    // 成功返回插件实例, 失败返回 nullptr(可用 lastError() 取原因)。
    JZPlugin* load(const std::string& path);

    // 卸载并销毁指定插件(传 load() 返回的实例)。
    void unload(JZPlugin* plugin);

    // 卸载全部已加载插件。
    void unloadAll();

    // 当前已加载的插件实例列表。
    const std::vector<JZPlugin*>& plugins() const { return m_plugins; }

    // 最近一次失败的原因(成功调用后不清空, 仅失败时更新)。
    const std::string& lastError() const { return m_lastError; }

private:
    struct PluginEntry
    {
        JZPlugin*          plugin  = nullptr;
        void*              handle  = nullptr;  // 平台动态库句柄
        JZPluginDestroyFunc destroy = nullptr;
    };

    void closeEntry(PluginEntry& entry);

    std::vector<PluginEntry> m_entries;
    std::vector<JZPlugin*>   m_plugins;
    std::string              m_lastError;
};

#endif  // JZ_PLUGIN_H_
