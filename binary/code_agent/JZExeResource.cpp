#include "JZExeResource.h"

// 注意：本文件只含 Win32 资源 API，绝不 include JZAgentTool.h（BOOLEAN 宏冲突）。
#include <Windows.h>

#include <fstream>
#include <vector>

// UTF-8 -> UTF-16（BeginUpdateResourceW / LoadLibraryExW 需要宽字符路径）
static std::wstring jzToWide(const std::string &s)
{
    if (s.empty())
        return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w;
    w.resize(n);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::vector<uint8_t> jzReadFile(const std::string &path, std::string &error)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        error = "无法打开文件: " + path;
        return {};
    }
    f.seekg(0, std::ios::end);
    std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf((size_t)(sz > 0 ? sz : 0));
    if (sz > 0)
        f.read((char *)buf.data(), sz);
    return buf;
}

const char *JZExeResource::kDebugType = "JZRT_CFG";
const char *JZExeResource::kDebugName = "DEBUG";

// ----------------------------------------------------------------------------
// 通用：任意资源读写
// ----------------------------------------------------------------------------
bool JZExeResource::setRawResource(const std::string &exePath,
                                   const std::string &type, const std::string &name,
                                   const std::string &data, std::string &error)
{
    error.clear();
    std::wstring wExe = jzToWide(exePath);
    std::wstring wType = jzToWide(type);
    std::wstring wName = jzToWide(name);

    HANDLE h = BeginUpdateResourceW(wExe.c_str(), FALSE);
    if (!h)
    {
        error = "BeginUpdateResource 失败（无法以资源更新方式打开目标 exe）: " + exePath;
        return false;
    }

    LANGID lang = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
    if (!UpdateResourceW(h, wType.c_str(), wName.c_str(), lang,
                         (void *)data.data(), (DWORD)data.size()))
    {
        EndUpdateResourceW(h, TRUE);
        error = "UpdateResource 失败（type=" + type + ", name=" + name + "）";
        return false;
    }

    if (!EndUpdateResourceW(h, FALSE))
    {
        error = "EndUpdateResource 失败（提交资源更新失败，目标未被修改）";
        return false;
    }
    return true;
}

bool JZExeResource::getRawResource(const std::string &exePath,
                                   const std::string &type, const std::string &name,
                                   std::string &out, std::string &error)
{
    error.clear();
    out.clear();
    std::wstring wExe = jzToWide(exePath);
    std::wstring wType = jzToWide(type);
    std::wstring wName = jzToWide(name);

    // 以 DATAFILE 方式加载，仅读资源不执行代码
    HMODULE h = LoadLibraryExW(wExe.c_str(), NULL, LOAD_LIBRARY_AS_DATAFILE);
    if (!h)
    {
        error = "无法以 DATAFILE 方式打开目标 exe: " + exePath;
        return false;
    }

    HRSRC hrsrc = FindResourceExW(h, wType.c_str(), wName.c_str(),
                                  MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL));
    if (!hrsrc)
    {
        FreeLibrary(h);
        error = "资源不存在（type=" + type + ", name=" + name + "）: " + exePath;
        return false;
    }

    HGLOBAL hg = LoadResource(h, hrsrc);
    if (!hg)
    {
        FreeLibrary(h);
        error = "LoadResource 失败";
        return false;
    }
    DWORD sz = SizeofResource(h, hrsrc);
    const uint8_t *p = (const uint8_t *)LockResource(hg);
    if (p && sz > 0)
        out.assign((const char *)p, (size_t)sz);
    FreeLibrary(h);
    return true;
}

// ----------------------------------------------------------------------------
// 图标
// ----------------------------------------------------------------------------
bool JZExeResource::replaceIcon(const std::string &exePath, const std::string &icoPath,
                                std::string &error)
{
    error.clear();

    std::vector<uint8_t> ico = jzReadFile(icoPath, error);
    if (ico.empty())
        return false;

    // ---- 解析 .ico 的 ICONDIR ----
    // ICONDIR: WORD reserved(=0), WORD type(=1), WORD count
    if (ico.size() < 6 || ico[2] != 1 || ico[3] != 0)
    {
        error = ".ico 文件格式错误（非图标文件）: " + icoPath;
        return false;
    }
    int count = (int)ico[4] | ((int)ico[5] << 8);
    if (count <= 0)
    {
        error = ".ico 不含任何图像: " + icoPath;
        return false;
    }
    // 每个 ICONDIRENTRY 16 字节，紧跟在 ICONDIR(6) 之后
    if (ico.size() < (size_t)(6 + count * 16))
    {
        error = ".ico 文件截断: " + icoPath;
        return false;
    }

    struct Entry
    {
        uint32_t offset;
        uint32_t size;
    };
    std::vector<Entry> entries;
    entries.reserve(count);
    const uint8_t *p = ico.data() + 6;
    for (int i = 0; i < count; i++, p += 16)
    {
        Entry e;
        e.size = (uint32_t)p[12] | ((uint32_t)p[13] << 8) | ((uint32_t)p[14] << 16) |
                 ((uint32_t)p[15] << 24);
        e.offset = (uint32_t)p[16] | ((uint32_t)p[17] << 8) | ((uint32_t)p[18] << 16) |
                   ((uint32_t)p[19] << 24);
        if (e.offset + e.size > ico.size())
        {
            error = ".ico 图像数据越界: " + icoPath;
            return false;
        }
        entries.push_back(e);
    }

    // ---- 打开资源更新（保留其它资源，bDeleteExistingResources=FALSE）----
    std::wstring wExe = jzToWide(exePath);
    HANDLE h = BeginUpdateResourceW(wExe.c_str(), FALSE);
    if (!h)
    {
        error = "BeginUpdateResource 失败（无法以资源更新方式打开目标 exe）: " + exePath;
        return false;
    }

    // RT_ICON 资源数据就是 .ico 内每幅图像那一段原始字节
    // （BITMAPINFOHEADER + 像素 + AND 掩码，或 PNG），可直接写入。
    LANGID lang = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
    for (int i = 0; i < count; i++)
    {
        const Entry &e = entries[i];
        if (!UpdateResourceW(h, MAKEINTRESOURCEW(3) /*RT_ICON*/, MAKEINTRESOURCEW(i + 1), lang,
                             (void *)(ico.data() + e.offset), e.size))
        {
            EndUpdateResourceW(h, TRUE);
            error = "UpdateResource(RT_ICON) 失败";
            return false;
        }
    }

    // ---- 构造 GRPICONDIR：ICONDIR + 每幅图的 GRPICONDIRENTRY ----
    // GRPICONDIRENTRY = ICONDIRENTRY 前 12 字节(width/height/colorCount/reserved/planes/bitCount)
    //                  + WORD id（替换原 offset/size 两字段）
    std::vector<uint8_t> grp;
    grp.resize((size_t)(6 + count * 14));
    grp[0] = 0;
    grp[1] = 0; // reserved
    grp[2] = 1;
    grp[3] = 0; // type = 1 (icon)
    grp[4] = (uint8_t)(count & 0xFF);
    grp[5] = (uint8_t)((count >> 8) & 0xFF);
    uint8_t *gp = grp.data() + 6;
    for (int i = 0; i < count; i++)
    {
        const uint8_t *src = ico.data() + 6 + i * 16;
        memcpy(gp, src, 12); // width,height,colorCount,reserved,planes,bitCount
        gp[12] = (uint8_t)((i + 1) & 0xFF);
        gp[13] = (uint8_t)(((i + 1) >> 8) & 0xFF);
        gp += 14;
    }
    if (!UpdateResourceW(h, MAKEINTRESOURCEW(14) /*RT_GROUP_ICON*/, MAKEINTRESOURCEW(1), lang,
                         grp.data(), (DWORD)grp.size()))
    {
        EndUpdateResourceW(h, TRUE);
        error = "UpdateResource(RT_GROUP_ICON) 失败";
        return false;
    }

    if (!EndUpdateResourceW(h, FALSE))
    {
        error = "EndUpdateResource 失败（提交资源更新失败，目标未被修改）";
        return false;
    }
    return true;
}

bool JZExeResource::hasIcon(const std::string &exePath)
{
    std::wstring w = jzToWide(exePath);
    // 以 DATAFILE 方式加载，仅查资源不执行代码
    HMODULE h = LoadLibraryExW(w.c_str(), NULL, LOAD_LIBRARY_AS_DATAFILE);
    if (!h)
        return false;
    HRSRC hrsrc = FindResourceExW(h, MAKEINTRESOURCEW(14) /*RT_GROUP_ICON*/, MAKEINTRESOURCEW(1),
                                  MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL));
    bool ok = (hrsrc != NULL);
    FreeLibrary(h);
    return ok;
}

// ----------------------------------------------------------------------------
// 调试开关资源
// ----------------------------------------------------------------------------
bool JZExeResource::setDebugFlag(const std::string &exePath, bool debug, std::string &error)
{
    // 内容用最小 JSON，避免引入 json 解析依赖：{"debug":true} / {"debug":false}
    std::string data = debug ? "{\"debug\":true}" : "{\"debug\":false}";
    return setRawResource(exePath, kDebugType, kDebugName, data, error);
}

bool JZExeResource::getDebugFlag(const std::string &exePath, bool &debug, std::string &error)
{
    debug = false;
    std::string data;
    if (!getRawResource(exePath, kDebugType, kDebugName, data, error))
        return false; // 资源不存在也算"成功读取"，调用方按 debug=false 处理

    // 极简解析：找到 "debug" 后取 true/false（不依赖 json 库）
    size_t pos = data.find("\"debug\"");
    if (pos == std::string::npos)
        return true;
    size_t colon = data.find(':', pos);
    if (colon == std::string::npos)
        return true;
    size_t valPos = data.find_first_not_of(" \t\r\n", colon + 1);
    if (valPos == std::string::npos)
        return true;
    if (data.compare(valPos, 4, "true") == 0)
        debug = true;
    else if (data.compare(valPos, 5, "false") == 0)
        debug = false;
    return true;
}
