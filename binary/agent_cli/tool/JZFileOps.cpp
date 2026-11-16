#include "JZFileOps.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "JZCodeTools.h"   // codeNormalizePath
#include "JZDir.h"
#include "JZFile.h"
#include "JZFileInfo.h"

using namespace std;

namespace JZFileOps
{

bool pathInRoot(const string &root, const string &p)
{
    return !root.empty() && !p.empty() &&
           (p == root ||
            (p.size() > root.size() && p.compare(0, root.size(), root) == 0 && p[root.size()] == '/'));
}

bool validEntryName(const string &name)
{
    return !name.empty() && name != "." && name != ".." &&
           name.find('/') == string::npos && name.find('\\') == string::npos;
}

// 父目录: "C:/a/b" -> "C:/a"; "C:/a" -> "C:/"; "C:/" -> ""
static string parentDirOf(const string &p)
{
    const size_t pos = p.find_last_of('/');
    if (pos == string::npos)
        return string();
    if (pos == 0)
        return "/";
    if (pos == 2 && p.size() >= 3 && p[1] == ':')
        return p.substr(0, 3);
    return p.substr(0, pos);
}

Json::Value list(const string &inputPath, const string &defaultDir, string &error)
{
    error.clear();

    string path = inputPath;
    if (path.empty())
        path = defaultDir;
    if (path.empty())
        path = JZDir::currentPath();
    path = codeNormalizePath(path);

    JZFileInfo info(path);
    if (!info.exists() || !info.isDir())
    {
        error = "目录不存在: " + path;
        return Json::Value();
    }

    Json::Value out(Json::objectValue);
    out["path"] = path;

    // 父目录: 到盘符根("C:/")或 "/" 就没了
    string parent = path;
    while (parent.size() > 1 && parent.back() == '/')
        parent.pop_back();
    const size_t pos = parent.find_last_of('/');
    if (pos == 0)
        parent = "/";
    else if (pos == 2 && parent.size() >= 3 && parent[1] == ':')
        parent = parent.substr(0, 3);
    else if (pos == string::npos && parent.size() == 2 && parent[1] == ':')
        parent = string();                       // "C:" 就是 C 盘根, 没有上级
    else if (pos != string::npos)
        parent = parent.substr(0, pos);
    out["parent"] = (parent == path) ? string() : parent;

    vector<string> names = JZDir(path).entryList(vector<string>(), JZDir::Dirs | JZDir::NoDotAndDotDot);
    std::sort(names.begin(), names.end());
    Json::Value dirs(Json::arrayValue);
    for (const string &n : names)
    {
        Json::Value d(Json::objectValue);
        d["name"] = n;
        d["path"] = path + "/" + n;
        dirs.append(d);
    }
    out["dirs"] = dirs;

    // 文件(文件树的只读浏览): 名字/大小/修改时间。内容不在这里, 走 read。
    vector<string> files = JZDir(path).entryList(vector<string>(), JZDir::Files | JZDir::NoDotAndDotDot);
    std::sort(files.begin(), files.end());
    Json::Value farr(Json::arrayValue);
    for (const string &n : files)
    {
        const string fp = path + "/" + n;
        JZFileInfo fi(fp);
        Json::Value f(Json::objectValue);
        f["name"] = n;
        f["path"] = fp;
        f["size"] = (Json::Int64)fi.size();
        f["mtime"] = (Json::Int64)fi.lastModified();
        farr.append(f);
        if (farr.size() >= 3000)      // 超大目录截断, 免得一次回几十 MB JSON
            break;
    }
    out["files"] = farr;

    // 根(盘符)列表: 网页上像 VSCode 的"打开文件夹"那样, 可以从别的盘开始。
    // 不引平台头文件, 直接试 A: .. Z: 是否存在(26 次 stat, 可忽略)。
    Json::Value drives(Json::arrayValue);
    for (char ch = 'A'; ch <= 'Z'; ++ch)
    {
        const string d = string(1, ch) + ":/";
        if (JZFileInfo(d).exists() && JZFileInfo(d).isDir())
            drives.append(d);
    }
    if (drives.empty())
        drives.append("/");
    out["drives"] = drives;

    // 主目录: 界面上放个"快捷入口"
    const char *home = getenv("USERPROFILE");
    if (!home || !*home)
        home = getenv("HOME");
    if (home && *home)
        out["home"] = codeNormalizePath(home);

    return out;
}

bool readFile(const string &inputPath, int64_t maxBytes, string &data,
              int64_t &size, int64_t &mtime, bool &truncated, string &error)
{
    error.clear();
    data.clear();
    size = 0;
    mtime = 0;
    truncated = false;

    const string path = codeNormalizePath(inputPath);
    JZFileInfo info(path);
    if (path.empty() || !info.exists() || info.isDir())
    {
        error = "文件不存在: " + path;
        return false;
    }

    if (maxBytes <= 0)
        maxBytes = 1024 * 1024;

    JZFile file;
    if (!file.open(path, "rb"))
    {
        error = "打不开(权限?): " + path;
        return false;
    }
    JZByteArray bytes = file.read((int)maxBytes);
    file.close();

    data.assign(bytes.data(), (size_t)bytes.size());
    size = (int64_t)info.size();
    mtime = (int64_t)info.lastModified();
    truncated = size > (int64_t)data.size();
    return true;
}

bool create(const string &inputRoot, const string &inputDir, const string &name,
            bool isDir, string &outPath, string &error)
{
    error.clear();
    outPath.clear();

    const string root = codeNormalizePath(inputRoot);
    const string dir = codeNormalizePath(inputDir);
    if (root.empty() || dir.empty() || name.empty())
    {
        error = "缺少 root / dir / name";
        return false;
    }
    if (!validEntryName(name))
    {
        error = "名字不能带路径分隔符, 也不能是 . 或 ..";
        return false;
    }
    if (!pathInRoot(root, dir))
    {
        error = "只能在工作区内新建\n工作区: " + root + "\n目标: " + dir;
        return false;
    }

    JZFileInfo dirInfo(dir);
    if (!dirInfo.exists() || !dirInfo.isDir())
    {
        error = "目录不存在: " + dir;
        return false;
    }

    const string target = dir + "/" + name;
    if (JZFileInfo(target).exists())
    {
        error = "已存在: " + name;
        return false;
    }

    bool ok = false;
    if (!isDir)
    {
        JZFile f;                       // "wb" 就是"建一个空文件"
        ok = f.open(target, "wb");
        if (ok)
            f.close();
    }
    else
    {
        ok = JZDir(dir).mkpath(target);
    }
    if (!ok)
    {
        error = "创建失败(权限?): " + target;
        return false;
    }

    outPath = target;
    return true;
}

bool renamePath(const string &inputRoot, const string &inputPath, const string &name,
                string &outPath, string &error)
{
    error.clear();
    outPath.clear();

    const string root = codeNormalizePath(inputRoot);
    const string path = codeNormalizePath(inputPath);
    if (root.empty() || path.empty() || name.empty())
    {
        error = "缺少 root / path / name";
        return false;
    }
    if (!pathInRoot(root, path))
    {
        error = "只能改工作区内的东西\n工作区: " + root;
        return false;
    }
    if (path == root)
    {
        error = "不能重命名工作区根目录";
        return false;
    }
    if (!validEntryName(name))
    {
        error = "名字不能带路径分隔符, 也不能是 . 或 ..";
        return false;
    }

    JZFileInfo info(path);
    if (!info.exists())
    {
        error = "不存在: " + path;
        return false;
    }

    const string parent = parentDirOf(path);
    const string target = parent + "/" + name;
    if (target == path)
    {
        outPath = target;
        return true;                    // 名字没变: 当作成功
    }
    if (JZFileInfo(target).exists())
    {
        error = "已存在: " + name;
        return false;
    }

    string err;
    const bool ok = info.isDir() ? JZDir::rename(path, target, &err)
                                 : JZFile::rename(path, target, &err);
    if (!ok)
    {
        error = "重命名失败" + (err.empty() ? string() : (": " + err));
        return false;
    }
    outPath = target;
    return true;
}

bool remove(const string &inputRoot, const string &inputPath, string &error)
{
    error.clear();

    const string root = codeNormalizePath(inputRoot);
    const string path = codeNormalizePath(inputPath);
    if (root.empty() || path.empty())
    {
        error = "缺少 root / path";
        return false;
    }
    if (!pathInRoot(root, path))
    {
        error = "只能删工作区内的东西\n工作区: " + root;
        return false;
    }
    if (path == root)
    {
        error = "不能删除工作区根目录";
        return false;
    }

    JZFileInfo info(path);
    if (!info.exists())
    {
        error = "不存在: " + path;
        return false;
    }

    const bool isDir = info.isDir();
    const bool ok = isDir ? JZDir(path).removeRecursively() : JZFile::remove(path);
    if (!ok)
    {
        error = "删除失败(权限?): " + path;
        return false;
    }
    return true;
}

} // namespace JZFileOps
