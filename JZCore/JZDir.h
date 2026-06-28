#ifndef JZ_DIR_H_
#define JZ_DIR_H_

#include "JZStd.h"
#include "JZFileInfo.h"

class JZDir
{
public:
    // 过滤选项
    enum DirFilter 
    {
        Files = 0x01,           // 列出文件
        Dirs = 0x02,            // 列出目录
        NoDotAndDotDot = 0x04   // 不列出 "." 和 ".."
    };

    // 排序选项
    enum DirSort 
    {
        SortNone = 0x0,
        SortName = 0x01, // 按名称排序
        SortSize = 0x02, // 按大小排序
        SortTime = 0x04, // 按修改时间排序
        SortReversed = 0x08  // 反向排序
    };

    static bool isAbsolutePath(std::string path);
    static std::string currentPath();

    JZDir();
    JZDir(const std::string &path);

    std::string name() const;

    bool exists() const;
    bool exists(const std::string &path) const;

    bool mkdir(const std::string &path);
    bool rmdir(const std::string &path);
    bool mkpath(const std::string &path);

    bool remove(const std::string &fileName);
    bool removeRecursively();

    void setPath(const std::string &path);
    std::string path() const;
    std::string absolutePath() const;
    std::string absoluteFilePath(std::string path) const;

    // 获取目录中所有文件信息
    std::vector<JZFileInfo> entryInfoList(int filter = Files | Dirs | NoDotAndDotDot, int sort = SortNone) const;
    std::vector<JZFileInfo> entryInfoList(const std::vector<std::string> &filters,int filter, int sort = SortNone) const;
    std::vector<std::string> entryList(int filter = Files | Dirs | NoDotAndDotDot,int sort = SortNone) const;
    std::vector<std::string> entryList(const std::vector<std::string> &filters, int filter = Files | Dirs | NoDotAndDotDot,int sort = SortNone) const;

private:
    bool dirExists(const std::string &path) const;
    bool createDir(const std::string &path) const;
    bool removeDirRev(const std::string &path) const;

    std::string m_path;
};
#endif