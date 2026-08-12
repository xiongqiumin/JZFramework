#ifndef JZ_FILE_INFO_H_
#define JZ_FILE_INFO_H_

#include <string>
#include "JZGlobal.h"

class JZFileInfo 
{
public:
    static bool exists(const std::string &path);
    static std::string weaklyCanonicalPath(std::string path);

    JZFileInfo();
    JZFileInfo(const std::string &path);

    void setFile(const std::string &path);

    bool exists() const;
    bool isFile() const;
    bool isDir()  const;

    std::string fileName() const;
    std::string filePath() const;
    std::string path() const;

    std::string baseName() const;
    std::string suffix() const;   //不带小数点

    int size() const;
    int64_t lastModified() const;

private:
    std::string m_path;
    bool m_exists;
    bool m_isFile;
    bool m_isDir;
    int m_size;
    int64_t m_time;
};











#endif // !JZ_FILE_INFO_H_
