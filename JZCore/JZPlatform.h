#ifndef JZ_PLAT_FORM_H_
#define JZ_PLAT_FORM_H_
#include <string>
#include <vector>
#include <cstddef>
#include "JZGlobal.h"

struct JZFRAMEWORK_API JZFileStat
{
    bool exists = false;
    bool isFile = false;
    bool isDir  = false;
    int size = 0;
    int64_t lastModified = 0;  // 毫秒时间戳
};

class JZFRAMEWORK_API JZPlatformWrapper
{
public:
	virtual ~JZPlatformWrapper();

	virtual int64_t getTickCountNs() = 0;

	// === 内存分配 ===
	// 默认走标准 malloc/free; 具体平台可重写(如 esp32 从 PSRAM 分配)
	virtual void* malloc(size_t size);
	virtual void  free(void *ptr);

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

	// === 环境变量 ===
	// 桌面/Android 走 OS 的实现; 裸机默认没有这个概念 —— 返回空串/false。
	// 真要在板上支持(如存 NVS), 由具体平台驱动重写这两个虚函数即可。
	virtual std::string getEnv(const std::string &name);
	virtual bool setEnv(const std::string &name, const std::string &value);
};

class JZFRAMEWORK_API JZPlatform
{
public:
	static void setInstance(JZPlatformWrapper *inst);
    static JZPlatformWrapper *instance();

    static int64_t getTickCountMs();
	static int64_t getTickCountNs();

	// 内存分配: 有平台实例时走实例(如 esp32 PSRAM), 否则退化到标准 malloc/free
	static void* malloc(size_t size);
	static void  free(void *ptr);

	// 环境变量(有平台实例时优先问实例):
	//   getEnv: 不存在/平台不支持 -> 空串
	//   setEnv: value 为空 = 删除该变量(对齐 Win32 语义, POSIX 侧也用 unsetenv); 返回是否成功
	// ⚠ OS 的 getenv/setenv 本身不是线程安全的, 只在启动阶段读写; 运行中要频繁改请自己加锁。
	static std::string getEnv(const std::string &name);
	static bool setEnv(const std::string &name, const std::string &value);

	static std::string platform();
	static std::string applicationFilePath();
	static std::string applicationDirPath();
	static std::string applicationTmpPath();
};

#endif // !PALT_FORM_H_
