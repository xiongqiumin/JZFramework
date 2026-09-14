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

    // 设置进程当前工作目录 (path 为 UTF-8)。
    // 失败(目录不存在/无权限/平台不支持)返回 false, 当前目录保持不变。
    // 命名与上面的 currentPath() 对称 —— 这里**有意偏离** Qt: QDir 的静态 setter 叫 setCurrent()
    // (Qt 那一对本来不对称, 实例侧才是 path()/setPath()); 本框架静态这一对统一用 xxxPath 后缀。
    static bool setCurrentPath(const std::string &path);

    // 递归拷贝目录（含子目录）。Qt 没有对应的 API，取名与已有的 removeRecursively()
    // 对称。src 必须是目录；dst 不存在会 mkpath 出来，已存在则合并进去（cp -r 语义）。
    // force: 目标下已存在同名**文件**时是否覆盖
    //   false(默认行为，与 QFile::copy 一致) -> 撞见同名文件直接失败，不覆盖；
    //   true -> 覆盖同名文件（目录仍合并，不删除目标）。
    // 中途失败不回滚（保留已拷部分），错误信息写入 *error（可为 nullptr）。
    static bool copyRecursively(const std::string &srcPath, const std::string &dstPath,
                                bool force, std::string *error = nullptr);

    // 重命名 / 移动目录（对应 QDir::rename），内部走 JZFile::rename。
    static bool rename(const std::string &oldPath, const std::string &newPath,
                       std::string *error = nullptr);

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

    bool isResource() const;   // 路径以 ':' 开头, 指向 JZResources 中的资源组

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