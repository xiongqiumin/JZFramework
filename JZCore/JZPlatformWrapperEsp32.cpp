#include "JZPlatformWrapperEsp32.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>

JZPlatformWrapperEsp32::~JZPlatformWrapperEsp32()
{
}
// ==================== 时间 ====================

int64_t JZPlatformWrapperEsp32::getTickCountNs()
{
    int64_t now_us = esp_timer_get_time();
    return now_us * 1000;
}

// ==================== 内存分配 ====================
void* JZPlatformWrapperEsp32::malloc(size_t size)
{
#ifdef CONFIG_SPIRAM
    // 优先PSRAM，PSRAM不足自动分配内部SRAM，一次调用完成
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_INTERNAL);
#else
    // 无PSRAM硬件/未开启，仅内部内存
    return heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
#endif
}

void JZPlatformWrapperEsp32::free(void *ptr)
{
    // heap_caps_free 可释放 PSRAM 与内部内存
    heap_caps_free(ptr);
}

// ==================== 文件句柄操作 ====================

void* JZPlatformWrapperEsp32::fopen(const char *filepath, const char *mode)
{
    return ::fopen(filepath, mode);
}

int JZPlatformWrapperEsp32::fclose(void *fp)
{
    return ::fclose((FILE*)fp);
}

size_t JZPlatformWrapperEsp32::fread(void *buf, size_t size, void *fp)
{
    return ::fread(buf, 1, size, (FILE*)fp);
}

size_t JZPlatformWrapperEsp32::fwrite(const void *buf, size_t size, void *fp)
{
    return ::fwrite(buf, 1, size, (FILE*)fp);
}

int JZPlatformWrapperEsp32::fseek(void *fp, long pos, int origin)
{
    return ::fseek((FILE*)fp, pos, origin);
}

long JZPlatformWrapperEsp32::ftell(void *fp)
{
    return ::ftell((FILE*)fp);
}

int JZPlatformWrapperEsp32::feof(void *fp)
{
    return ::feof((FILE*)fp);
}

// ==================== 文件系统操作 ====================

JZFileStat JZPlatformWrapperEsp32::fileStat(const std::string &filepath)
{
    JZFileStat st;
    struct ::stat buf;
    if (::stat(filepath.c_str(), &buf) == 0)
    {
        st.exists = true;
        st.isFile = S_ISREG(buf.st_mode);
        st.isDir  = S_ISDIR(buf.st_mode);
        st.size   = (int)buf.st_size;
        st.lastModified = (int64_t)buf.st_mtime * 1000;
    }
    return st;
}

bool JZPlatformWrapperEsp32::fileRemove(const std::string &filepath)
{
    return ::unlink(filepath.c_str()) == 0;
}

bool JZPlatformWrapperEsp32::isAbsolutePath(const std::string &path)
{
    return !path.empty() && path[0] == '/';
}

std::string JZPlatformWrapperEsp32::currentPath()
{
    char buf[256];
    if (::getcwd(buf, sizeof(buf)))
        return buf;
    return "/";
}

bool JZPlatformWrapperEsp32::dirCreate(const std::string &path)
{
    return ::mkdir(path.c_str(), 0755) == 0;
}

bool JZPlatformWrapperEsp32::dirRemove(const std::string &path)
{
    return ::rmdir(path.c_str()) == 0;
}

std::vector<std::string> JZPlatformWrapperEsp32::dirEntryList(const std::string &path)
{
    std::vector<std::string> result;
    DIR *dir = ::opendir(path.c_str());
    if (!dir) return result;

    struct dirent *entry;
    while ((entry = ::readdir(dir)) != nullptr)
    {
        result.push_back(entry->d_name);
    }
    ::closedir(dir);
    return result;
}
