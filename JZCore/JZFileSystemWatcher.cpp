#include "JZFileSystemWatcher.h"

// 实现说明见头文件注释；仅 Windows 下 ReadDirectoryChangesW 真正生效。
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600   // CancelIoEx 等需要 Vista+
#endif

#include <windows.h>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <cctype>
#include <atomic>
#include "JZDir.h"
#include "JZFileInfo.h"
#include "JZStd.h"

JZ_OBJECT_IMPL(JZFileSystemWatcher, JZObject)

namespace {

std::string normalizePath(const std::string& p)
{
    std::string s = p;
    for (char& c : s) if (c == '\\') c = '/';
    if (!s.empty() && s.back() != '/') s += '/';
    return s;
}

std::string toLower(std::string s)
{
    for (char& c : s) c = (char)::tolower((unsigned char)c);
    return s;
}

std::string parentDirOf(const std::string& filePath)
{
    std::string norm = filePath;
    for (char& c : norm) if (c == '\\') c = '/';
    auto pos = norm.find_last_of('/');
    if (pos == std::string::npos) return ".";
    return norm.substr(0, pos);
}

std::string baseNameOf(const std::string& path)
{
    std::string s = path;
    for (char& c : s) if (c == '\\') c = '/';
    auto pos = s.find_last_of('/');
    return (pos == std::string::npos) ? s : s.substr(pos + 1);
}

} // namespace

struct JZFileSystemWatcher::WatchItem
{
    HANDLE hDir = INVALID_HANDLE_VALUE;
    OVERLAPPED ol = {};
    HANDLE hEvent = nullptr;
    std::vector<BYTE> buffer;
    std::string dirPath;            // 规范化监控目录(结尾 '/')
    bool watchAll = false;          // true=监视整目录；false=仅 files 集合内的文件
    std::set<std::string> files;    // watchAll=false 时关注的文件名(小写)
    std::atomic<bool> stop{false};
    std::thread* thread = nullptr;
    JZFileSystemWatcher* owner = nullptr;
};

JZFileSystemWatcher::JZFileSystemWatcher()
    : sigFileChanged(this)
    , sigFileAdded(this)
    , sigFileRemoved(this)
    , sigFileModified(this)
    , sigFileRenamed(this)
{}
JZFileSystemWatcher::~JZFileSystemWatcher() { clear(); }

bool JZFileSystemWatcher::addPath(const std::string& path)
{
    JZFileInfo fi(path);
    if (!fi.exists())
        return false;

    bool isDir = fi.isDir();
    std::string watchDir = isDir ? JZDir(path).absolutePath()
                                 : JZDir(parentDirOf(path)).absolutePath();
    std::string fileName = isDir ? std::string() : fi.fileName();

    std::string dirNorm = normalizePath(watchDir);
    std::string key = toLower(dirNorm);

    {
        JZMutexLocker lock(&m_mutex);
        auto it = m_watches.find(key);
        if (it != m_watches.end())
        {
            WatchItem* item = it->second;
            if (isDir) item->watchAll = true;
            else if (!fileName.empty()) item->files.insert(toLower(fileName));
            return true;
        }
    }

    WatchItem* item = new WatchItem;
    item->owner = this;
    item->dirPath = dirNorm;
    if (isDir) item->watchAll = true;
    else if (!fileName.empty()) item->files.insert(toLower(fileName));
    item->buffer.resize(64 * 1024);

    std::wstring wdir = str_to_wstr(watchDir);
    item->hDir = CreateFileW(wdir.c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (item->hDir == INVALID_HANDLE_VALUE) { delete item; return false; }

    item->hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (!item->hEvent) { CloseHandle(item->hDir); delete item; return false; }
    item->ol.hEvent = item->hEvent;

    {
        JZMutexLocker lock(&m_mutex);
        m_watches[key] = item;
    }
    item->thread = new std::thread(watchLoop, item);
    return true;
}

bool JZFileSystemWatcher::removePath(const std::string& path)
{
    JZFileInfo fi(path);
    bool isDir = fi.isDir();
    std::string watchDir = isDir ? JZDir(path).absolutePath()
                                 : JZDir(parentDirOf(path)).absolutePath();
    std::string fileName = isDir ? std::string() : fi.fileName();

    std::string dirNorm = normalizePath(watchDir);
    std::string key = toLower(dirNorm);

    WatchItem* item = nullptr;
    {
        JZMutexLocker lock(&m_mutex);
        auto it = m_watches.find(key);
        if (it == m_watches.end())
            return false;
        item = it->second;

        if (isDir)
        {
            m_watches.erase(it);
        }
        else
        {
            if (!fileName.empty()) item->files.erase(toLower(fileName));
            if (!item->watchAll && item->files.empty())
                m_watches.erase(it);
            else
                return true;   // 仍被监视，线程继续运行
        }
    }

    // 真正需要停止该目录监视
    item->stop = true;
    if (item->hDir != INVALID_HANDLE_VALUE)
        CancelIoEx(item->hDir, &item->ol);   // 唤醒挂起的 ReadDirectoryChangesW
    if (item->thread) { item->thread->join(); delete item->thread; }
    if (item->hEvent) CloseHandle(item->hEvent);
    if (item->hDir != INVALID_HANDLE_VALUE) CloseHandle(item->hDir);
    delete item;
    return true;
}

void JZFileSystemWatcher::clear()
{
    std::vector<WatchItem*> items;
    {
        JZMutexLocker lock(&m_mutex);
        for (auto& kv : m_watches) items.push_back(kv.second);
        m_watches.clear();
    }
    for (WatchItem* item : items)
    {
        item->stop = true;
        if (item->hDir != INVALID_HANDLE_VALUE)
            CancelIoEx(item->hDir, &item->ol);
        if (item->thread) { item->thread->join(); delete item->thread; }
        if (item->hEvent) CloseHandle(item->hEvent);
        if (item->hDir != INVALID_HANDLE_VALUE) CloseHandle(item->hDir);
        delete item;
    }
}

bool JZFileSystemWatcher::contains(const std::string& path) const
{
    JZFileInfo fi(path);
    bool isDir = fi.isDir();
    std::string watchDir = isDir ? JZDir(path).absolutePath()
                                 : JZDir(parentDirOf(path)).absolutePath();
    std::string key = toLower(normalizePath(watchDir));
    JZMutexLocker lock(&m_mutex);
    auto it = m_watches.find(key);
    if (it == m_watches.end())
        return false;
    if (isDir)
        return it->second->watchAll;
    std::string fileName = fi.fileName();
    return !it->second->watchAll && it->second->files.count(toLower(fileName)) > 0;
}

void JZFileSystemWatcher::processEvents(WatchItem* item, uint32_t bytes)
{
    BYTE* p = item->buffer.data();
    BYTE* end = p + bytes;
    while (p < end)
    {
        FILE_NOTIFY_INFORMATION* fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(p);
        std::wstring wname(fni->FileName, fni->FileNameLength / sizeof(WCHAR));
        std::string name = wstr_to_str(wname);
        for (char& c : name) if (c == '\\') c = '/';
        std::string full = item->dirPath + name;
        std::string base = toLower(baseNameOf(name));

        bool interested = item->watchAll || item->files.count(base) > 0;
        if (interested)
        {
            switch (fni->Action)
            {
            case FILE_ACTION_ADDED:
                sigFileAdded.emit(full); sigFileChanged.emit(full); break;
            case FILE_ACTION_REMOVED:
                sigFileRemoved.emit(full); sigFileChanged.emit(full); break;
            case FILE_ACTION_MODIFIED:
                sigFileModified.emit(full); sigFileChanged.emit(full); break;
            case FILE_ACTION_RENAMED_OLD_NAME:
                sigFileRemoved.emit(full); sigFileChanged.emit(full); break;
            case FILE_ACTION_RENAMED_NEW_NAME:
                sigFileAdded.emit(full); sigFileRenamed.emit(full); sigFileChanged.emit(full); break;
            default: break;
            }
        }
        if (fni->NextEntryOffset == 0) break;
        p += fni->NextEntryOffset;
    }
}

void JZFileSystemWatcher::watchLoop(WatchItem* item)
{
    const DWORD bufSize = (DWORD)item->buffer.size();
    while (!item->stop)
    {
        DWORD bytes = 0;
        BOOL ok = ReadDirectoryChangesW(item->hDir, item->buffer.data(), bufSize, TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION,
            &bytes, &item->ol, nullptr);
        if (!ok) break;   // 目录已失效/句柄错误

        DWORD res = WaitForSingleObject(item->hEvent, INFINITE);
        if (item->stop) break;
        if (res != WAIT_OBJECT_0) break;

        if (!GetOverlappedResult(item->hDir, &item->ol, &bytes, FALSE))
        {
            DWORD err = GetLastError();
            if (err == ERROR_OPERATION_ABORTED) break;  // 被 stop 取消(CancelIoEx)
            // ERROR_NOTIFY_ENUM_DIR 等：缓冲溢出可能丢失少量事件，重新发起监视
            ResetEvent(item->hEvent);
            continue;
        }
        if (bytes > 0)
            item->owner->processEvents(item, (uint32_t)bytes);
        ResetEvent(item->hEvent);
    }
}

#endif // _WIN32
