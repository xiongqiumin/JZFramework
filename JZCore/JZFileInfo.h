#ifndef JZ_FILE_INFO_H_
#define JZ_FILE_INFO_H_
#include <string>
#include "JZGlobal.h"

class JZFRAMEWORK_API JZFileInfo 
{
public:
    static bool exists(const std::string &path);
    static std::string weaklyCanonicalPath(std::string path);
    static bool isResourcePath(const std::string &path);   // 资源路径: 以 ':/' 开头

    JZFileInfo();
    JZFileInfo(const std::string &path);

    void setFile(const std::string &path);

    bool exists() const;
    bool isFile() const;
    bool isDir()  const;

    bool isResource() const;     // 路径以 ':' 开头, 指向 JZResources 注册的资源

    std::string fileName() const;
    std::string filePath() const;
    std::string path() const;

    std::string baseName() const;
    std::string suffix() const;   //不带小数点

    int size() const;
    int64_t lastModified() const;

private:
    void setResource(const std::string &path);

    std::string m_path;
    bool m_isResource;
    bool m_exists;
    bool m_isFile;
    bool m_isDir;
    int m_size;
    int64_t m_time;
};

#endif // !JZ_FILE_INFO_H_
