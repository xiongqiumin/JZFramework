#include "JZSharedMemory.h"

#include <cstring>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#  include <cerrno>
#endif

JZSharedMemory::JZSharedMemory(const std::string &key)
    : m_key(key)
{
}

JZSharedMemory::~JZSharedMemory()
{
    detach();
}

// 逻辑名 -> 平台名。key 里出现路径分隔符/反斜杠会破坏命名规则, 统一换成 '_'。
std::string JZSharedMemory::platformKey(const std::string &key)
{
    std::string k = key;
    for (size_t i = 0; i < k.size(); i++)
    {
        if (k[i] == '/' || k[i] == '\\' || k[i] == ':' || k[i] == ' ')
            k[i] = '_';
    }
#ifdef _WIN32
    return "Local\\JZSharedMemory_" + k;
#else
    return "/jz_" + k;
#endif
}

#ifdef _WIN32

bool JZSharedMemory::create(int size, AccessMode)
{
    detach();
    if (size <= 0)
    {
        m_error = "共享内存大小必须大于 0";
        return false;
    }
    const std::string name = platformKey(m_key);
    const DWORD protect = PAGE_READWRITE;
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, protect, 0, (DWORD)size,
                                  name.c_str());
    if (h == nullptr)
    {
        m_error = "创建共享内存失败（错误码 " + std::to_string((int)GetLastError()) + "）";
        return false;
    }
    // 已经有人建过同名段: create 视为失败(与 QSharedMemory 一致), 让调用方改用 attach
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(h);
        m_error = "共享内存已存在: " + m_key + "（用 attach() 打开）";
        return false;
    }
    void *p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (p == nullptr)
    {
        m_error = "映射共享内存失败（错误码 " + std::to_string((int)GetLastError()) + "）";
        CloseHandle(h);
        return false;
    }
    m_map = h;
    m_data = p;
    m_size = size;
    m_attached = true;
    m_error.clear();
    return true;
}

bool JZSharedMemory::attach(AccessMode mode)
{
    detach();
    const std::string name = platformKey(m_key);
    const DWORD access = (mode == ReadOnly) ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS;
    HANDLE h = OpenFileMappingA(access, FALSE, name.c_str());
    if (h == nullptr)
    {
        m_error = "共享内存不存在: " + m_key + "（错误码 "
                  + std::to_string((int)GetLastError()) + "）";
        return false;
    }
    void *p = MapViewOfFile(h, access, 0, 0, 0);
    if (p == nullptr)
    {
        m_error = "映射共享内存失败（错误码 " + std::to_string((int)GetLastError()) + "）";
        CloseHandle(h);
        return false;
    }
    // 段大小没有直接 API: 查这块映射所在的内存区域
    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T got = VirtualQuery(p, &mbi, sizeof(mbi));
    m_map = h;
    m_data = p;
    m_size = (got == sizeof(mbi)) ? (int)mbi.RegionSize : 0;
    m_attached = true;
    m_error.clear();
    return true;
}

bool JZSharedMemory::detach()
{
    if (m_data != nullptr)
    {
        UnmapViewOfFile(m_data);
        m_data = nullptr;
    }
    if (m_map != nullptr)
    {
        CloseHandle((HANDLE)m_map);
        m_map = nullptr;
    }
    m_attached = false;
    m_size = 0;
    return true;
}

bool JZSharedMemory::remove(const std::string &key)
{
    // Windows 的命名映射没有 "unlink": 名字随**最后一个句柄关闭**自动消失。
    // 这里主动打开再关一次, 只是为了在"没人用"时提前把名字释放掉。
    const std::string name = platformKey(key);
    HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, name.c_str());
    if (h != nullptr)
        CloseHandle(h);
    return true;
}

#elif defined(__linux__)

bool JZSharedMemory::create(int size, AccessMode)
{
    detach();
    if (size <= 0)
    {
        m_error = "共享内存大小必须大于 0";
        return false;
    }
    const std::string name = platformKey(m_key);
    int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0)
    {
        if (errno == EEXIST)
            m_error = "共享内存已存在: " + m_key + "（用 attach() 打开）";
        else
            m_error = std::string("创建共享内存失败: ") + ::strerror(errno);
        return false;
    }
    if (::ftruncate(fd, (off_t)size) != 0)
    {
        m_error = std::string("设置共享内存大小失败: ") + ::strerror(errno);
        ::close(fd);
        ::shm_unlink(name.c_str());
        return false;
    }
    void *p = ::mmap(nullptr, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED)
    {
        m_error = std::string("映射共享内存失败: ") + ::strerror(errno);
        ::close(fd);
        ::shm_unlink(name.c_str());
        return false;
    }
    m_fd = fd;
    m_data = p;
    m_size = size;
    m_attached = true;
    m_error.clear();
    return true;
}

bool JZSharedMemory::attach(AccessMode mode)
{
    detach();
    const std::string name = platformKey(m_key);
    int fd = ::shm_open(name.c_str(), (mode == ReadOnly) ? O_RDONLY : O_RDWR, 0);
    if (fd < 0)
    {
        m_error = (errno == ENOENT)
                      ? ("共享内存不存在: " + m_key)
                      : (std::string("打开共享内存失败: ") + ::strerror(errno));
        return false;
    }
    struct stat st;
    if (::fstat(fd, &st) != 0 || st.st_size <= 0)
    {
        m_error = "共享内存大小无效";
        ::close(fd);
        return false;
    }
    const int prot = (mode == ReadOnly) ? PROT_READ : (PROT_READ | PROT_WRITE);
    void *p = ::mmap(nullptr, (size_t)st.st_size, prot, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED)
    {
        m_error = std::string("映射共享内存失败: ") + ::strerror(errno);
        ::close(fd);
        return false;
    }
    m_fd = fd;
    m_data = p;
    m_size = (int)st.st_size;
    m_attached = true;
    m_error.clear();
    return true;
}

bool JZSharedMemory::detach()
{
    if (m_data != nullptr)
    {
        ::munmap(m_data, (size_t)m_size);
        m_data = nullptr;
    }
    if (m_fd >= 0)
    {
        ::close(m_fd);
        m_fd = -1;
    }
    m_attached = false;
    m_size = 0;
    return true;
}

bool JZSharedMemory::remove(const std::string &key)
{
    return ::shm_unlink(platformKey(key).c_str()) == 0;
}

#else

// 其它平台(嵌入式): 暂不支持跨进程共享内存
bool JZSharedMemory::create(int, AccessMode)
{
    m_error = "当前平台不支持共享内存";
    return false;
}

bool JZSharedMemory::attach(AccessMode)
{
    m_error = "当前平台不支持共享内存";
    return false;
}

bool JZSharedMemory::detach()
{
    return true;
}

bool JZSharedMemory::remove(const std::string &)
{
    return false;
}

#endif
