#pragma once
#include "JZPlatform.h"

class JZFRAMEWORK_API JZPlatformWrapperEsp32 : public JZPlatformWrapper
{
public:
    virtual ~JZPlatformWrapperEsp32() override;

    int64_t getTickCountNs() override;

    // === 内存分配(优先 PSRAM) ===
    void* malloc(size_t size) override;
    void  free(void *ptr) override;

    // === 文件句柄操作 ===
    void* fopen(const char *filepath, const char *mode) override;
    int fclose(void *fp) override;
    size_t fread(void *buf, size_t size, void *fp) override;
    size_t fwrite(const void *buf, size_t size, void *fp) override;
    int fseek(void *fp, long pos, int origin) override;
    long ftell(void *fp) override;
    int feof(void *fp) override;

    // === 文件系统操作 ===
    JZFileStat fileStat(const std::string &filepath) override;
    bool fileRemove(const std::string &filepath) override;
    bool isAbsolutePath(const std::string &path) override;
    std::string currentPath() override;
    bool dirCreate(const std::string &path) override;
    bool dirRemove(const std::string &path) override;
    std::vector<std::string> dirEntryList(const std::string &path) override;
};
