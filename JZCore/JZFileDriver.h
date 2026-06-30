#ifndef JZ_FILE_DRIVER_H_
#define JZ_FILE_DRIVER_H_

#include <string>
#include <vector>
#include <sys/stat.h>
#include "JZStd.h"

class JZFileAbstractDriver
{
public:
    JZFileAbstractDriver() = default;
    virtual ~JZFileAbstractDriver() = default;

    static JZFileAbstractDriver *instance();
    static void setInstance(JZFileAbstractDriver *inst);

    virtual bool isAbsolutePath(const std::string &path) = 0;
    virtual std::string currentPath() = 0;

    // stat: 返回 0 成功, -1 失败（path 不存在）
    virtual int stat(const std::string &path, struct stat *st) = 0;

    virtual bool fileRemove(const std::string &path) = 0;
    virtual bool dirCreate(const std::string &path) = 0;
    virtual bool dirRemove(const std::string &path) = 0;

    virtual std::vector<std::string> dirEntryList(const std::string &path) = 0;

protected:
    JZ_DISABLE_COPY(JZFileAbstractDriver)
    static JZFileAbstractDriver *s_instance;
};

#endif // JZ_FILE_DRIVER_H_
