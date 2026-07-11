#ifndef JZ_PLAT_FORM_H_
#define JZ_PLAT_FORM_H_

#include <string>
#include <vector>
#include "JZGlobal.h"

struct JZFileStat
{
    bool exists = false;
    bool isFile = false;
    bool isDir  = false;
    int size = 0;
    int64_t lastModified = 0;  // 毫秒时间戳
};

class JZPlatformWrapper
{
public:
	virtual ~JZPlatformWrapper() = default;

	virtual int64_t getTickCountNs() = 0;

	// === 文件句柄操作 ===
	virtual void* fopen(const char *filepath, const char *mode) = 0;
	virtual int fclose(void *fp) = 0;
	virtual size_t fread(void *buf, size_t size, void *fp) = 0;
	virtual size_t fwrite(const void *buf, size_t size, void *fp) = 0;
	virtual int fseek(void *fp, long pos, int origin) = 0;
	virtual long ftell(void *fp) = 0;
	virtual int feof(void *fp) = 0;

	// === 文件系统操作 ===
	virtual JZFileStat fileStat(const std::string &filepath) = 0;
	virtual bool fileRemove(const std::string &filepath) = 0;
	virtual bool isAbsolutePath(const std::string &path) = 0;
	virtual std::string currentPath() = 0;
	virtual bool dirCreate(const std::string &path) = 0;
	virtual bool dirRemove(const std::string &path) = 0;
	virtual std::vector<std::string> dirEntryList(const std::string &path) = 0;
};

class JZPlatform
{
public:
	static void setInstance(JZPlatformWrapper *inst);
    static JZPlatformWrapper *instance();

    static int64_t getTickCountMs();
	static int64_t getTickCountNs();

	static std::string platform();
	static std::string applicationFilePath();
	static std::string applicationDirPath();
	static std::string applicationTmpPath();
};

#endif // !PALT_FORM_H_
