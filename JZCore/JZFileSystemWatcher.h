#ifndef JZ_FILE_SYSTEM_WATCHER_H
#define JZ_FILE_SYSTEM_WATCHER_H
#include <string>
#include <set>
#include <unordered_map>
#include <cstdint>
#include "JZObject.h"
#include "JZSignal.h"
#include "JZMutex.h"

// 基于 Windows ReadDirectoryChangesW 的文件系统监视器。
//  - 仅在 Windows 平台提供真实实现；其它平台为兼容空壳（接口一致、不触发任何信号）。
//  - 监视目录（可递归子树）的文件增/删/改/重命名，并通过信号通知。
//  - ⚠ 非 Windows 下是空壳、且事件本身也可能丢（缓冲区溢出、网络盘、事件延迟），
//    所以**不要拿它做正确性保证**（例如靠它失效缓存）；它只适合做"尽力而为"的刷新提示。
//  - 当前全仓无使用者（此前的文件内容 LRU 缓存已删除）。
#ifdef _WIN32

class JZFRAMEWORK_API JZFileSystemWatcher : public JZObject
{
    JZ_OBJECT_DECLARE(JZFileSystemWatcher)
public:
    JZFileSystemWatcher();
    ~JZFileSystemWatcher();

    // path 可为目录或文件：
    //  - 目录 -> 递归监视其下所有变更；
    //  - 文件 -> 监视父目录，仅在该文件变更时发信号（同目录其它文件忽略）。
    // 返回是否成功开始/加入监视。
    bool addPath(const std::string& path);
    bool removePath(const std::string& path);
    void clear();
    bool contains(const std::string& path) const;

jz_signals:
    JZSignal<std::string> sigFileChanged;   // 任意变更(增/删/改/重命名)，用于缓存失效
    JZSignal<std::string> sigFileAdded;
    JZSignal<std::string> sigFileRemoved;
    JZSignal<std::string> sigFileModified;
    JZSignal<std::string> sigFileRenamed;   // 重命名后的新路径

private:
    struct WatchItem;
    void processEvents(WatchItem* item, uint32_t bytes);
    static void watchLoop(WatchItem* item);

    mutable JZMutex m_mutex;
    std::unordered_map<std::string, WatchItem*> m_watches;  // key = 规范化目录(小写)
};

#else // !_WIN32

// 非 Windows 平台占位实现：当前不参与实际监视，仅提供兼容接口。
class JZFRAMEWORK_API JZFileSystemWatcher : public JZObject
{
public:
    JZFileSystemWatcher()
        : sigFileChanged(this), sigFileAdded(this), sigFileRemoved(this)
        , sigFileModified(this), sigFileRenamed(this)
    {}
    ~JZFileSystemWatcher() {}

    bool addPath(const std::string&) { return false; }
    bool removePath(const std::string&) { return false; }
    void clear() {}
    bool contains(const std::string&) const { return false; }

jz_signals:
    JZSignal<std::string> sigFileChanged;
    JZSignal<std::string> sigFileAdded;
    JZSignal<std::string> sigFileRemoved;
    JZSignal<std::string> sigFileModified;
    JZSignal<std::string> sigFileRenamed;
};

#endif // _WIN32

#endif // JZ_FILE_SYSTEM_WATCHER_H
