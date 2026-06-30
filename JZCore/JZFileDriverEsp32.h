#ifndef JZ_FILE_DRIVER_ESP32_H_
#define JZ_FILE_DRIVER_ESP32_H_

#include "JZFileDriver.h"

class JZFileDriverEsp32 : public JZFileAbstractDriver
{
public:
    JZFileDriverEsp32();
    ~JZFileDriverEsp32() override;

    bool isAbsolutePath(const std::string &path) override;
    std::string currentPath() override;

    int stat(const std::string &path, struct stat *st) override;

    bool fileRemove(const std::string &path) override;
    bool dirCreate(const std::string &path) override;
    bool dirRemove(const std::string &path) override;

    std::vector<std::string> dirEntryList(const std::string &path) override;
};

#endif // JZ_FILE_DRIVER_ESP32_H_
