#ifndef JZ_PLAT_FORM_H_
#define JZ_PLAT_FORM_H_

#include <string>
#include "JZGlobal.h"

class JZPlatform
{
public:
    static int64_t getTickCountMs();  // 得到系统启动时间,单调递增(毫秒)

	static std::string platform();
	static std::string applicationFilePath();
	static std::string applicationDirPath();
	static std::string applicationTmpPath();
};

#endif // !PALT_FORM_H_
