#ifndef JZ_PLAT_FORM_H_
#define JZ_PLAT_FORM_H_

#include <string>
#include "JZGlobal.h"

class JZPlatformWrapper
{
public:
	virtual ~JZPlatformWrapper(){}

	virtual int64_t getTickCountNs() = 0;
};

class JZPlatform
{
public:
	static void setInstance(JZPlatformWrapper *inst);
    static JZPlatformWrapper *instance();

    static int64_t getTickCountMs();  // 得到系统启动时间,单调递增
	static int64_t getTickCountNs();

	static std::string platform();
	static std::string applicationFilePath();
	static std::string applicationDirPath();
	static std::string applicationTmpPath();
};

#endif // !PALT_FORM_H_
