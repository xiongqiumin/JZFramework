// ⚠ windows.h 要在最前（有保护宏），下面所有框架头都不用再操心 min/max 与 API 宏
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#endif

#include "JZEmulatorCore.h"

#ifdef _WIN32
#  include <windows.h>
#endif

namespace
{
    // 宿主 DLL 的名字（与 E:/work/simulator/host/CMakeLists.txt 的 OUTPUT_NAME 一致）
    const char* kDllName = "mgba_host.dll";

    // mGBA 的 GBA 视频缓冲固定 240x160（GB/GBC 更小）—— 当作上界用来校验回包
    const int kMaxFrameW = 240;
    const int kMaxFrameH = 160;

    // UTF-8 -> ANSI(CP_ACP)：宿主 DLL 是**窄字符/ANSI** 接口 —— 实测 `mgba_load_rom`
    // 收 UTF-8 路径直接返回 -2、收 mbcs(本机 GBK) 才成功；而框架里的字符串一律 UTF-8。
    // 所以在这一层做一次转换（中文 ROM 名这类 ACP 表示得了的走这条就够；ACP 表示不了的
    // 名字要支持，得让宿主 DLL 改收宽字符 —— 那属于 E:/work/simulator 那侧的事）。
    // ⚠ 同理，`GetFileAttributesA` 也是 ANSI 口：不转换就会"文件明明在却说找不到"。
    std::string to_ansi(const std::string& utf8)
    {
#ifdef _WIN32
        if (utf8.empty())
            return utf8;
        const int wn = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
        if (wn <= 0)
            return utf8;
        std::wstring w((size_t)wn, L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &w[0], wn);
        const int an = ::WideCharToMultiByte(CP_ACP, 0, w.c_str(), wn, nullptr, 0, nullptr, nullptr);
        if (an <= 0)
            return utf8;
        std::string a((size_t)an, '\0');
        ::WideCharToMultiByte(CP_ACP, 0, w.c_str(), wn, &a[0], an, nullptr, nullptr);
        return a;
#else
        return utf8;
#endif
    }

    bool file_exists(const std::string& path)
    {
#ifdef _WIN32
        return !path.empty() && ::GetFileAttributesA(to_ansi(path).c_str()) != INVALID_FILE_ATTRIBUTES;
#else
        return false;
#endif
    }
}

JZEmulatorCore::JZEmulatorCore()
{
    m_frameBuf.resize((size_t)kMaxFrameW * kMaxFrameH * 4);
}

JZEmulatorCore::~JZEmulatorCore()
{
    unload();
}

// ==================== DLL ====================
bool JZEmulatorCore::load(const std::string& dir)
{
    unload();
    m_error.clear();

#ifdef _WIN32
    std::string path = kDllName;
    if (!dir.empty())
    {
        // ⚠ 分隔符必须**统一成反斜杠**：实测 `LoadLibraryExA("E:\\...\\Debug/mgba_host.dll",
        //   ..., LOAD_WITH_ALTERED_SEARCH_PATH)` 会以**错误码 126**（找不到模块）失败 ——
        //   混合分隔符下那个"改变后的搜索路径"算不对，于是它的依赖（libgcc_s_seh-1.dll）
        //   找不到；全用正斜杠或全用反斜杠都正常。调用方给的是自然的 Windows 路径
        //   （反斜杠），所以在这里归一，别把这条暗坑留给每个调用方。
        std::string d = dir;
        while (!d.empty() && (d[d.size() - 1] == '\\' || d[d.size() - 1] == '/'))
            d.erase(d.size() - 1);
        for (size_t i = 0; i < d.size(); ++i)
        {
            if (d[i] == '/')
                d[i] = '\\';
        }
        path = d + "\\" + kDllName;
    }
    if (!file_exists(path))
    {
        m_error = "找不到 " + path;
        return false;
    }

    // ⚠ LOAD_WITH_ALTERED_SEARCH_PATH: 让这个 DLL 的依赖（MinGW 的
    //   libgcc_s_seh-1 / libstdc++-6 / libwinpthread-1）从**它自己所在目录**找，
    //   而不是 exe 目录/PATH —— 否则加载直接失败（实测踩过）。
    HMODULE mod = ::LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod)
    {
        m_error = "LoadLibraryEx 失败（错误码 " + std::to_string((long long)::GetLastError()) + "）: " + path;
        return false;
    }
    m_module = mod;

    m_createApi  = (FnCreate)::GetProcAddress(mod, "mgba_create");
    m_destroy    = (FnDestroy)::GetProcAddress(mod, "mgba_destroy");
    m_loadRom    = (FnLoadRom)::GetProcAddress(mod, "mgba_load_rom");
    m_runFrame   = (FnRunFrame)::GetProcAddress(mod, "mgba_run_frame");
    m_setKeys    = (FnSetKeys)::GetProcAddress(mod, "mgba_set_keys");
    m_getFrame   = (FnGetFrame)::GetProcAddress(mod, "mgba_get_frame");

    if (!m_createApi || !m_destroy || !m_loadRom || !m_runFrame || !m_setKeys || !m_getFrame)
    {
        m_error = kDllName + std::string(" 的导出符号不全（期望 mgba_create/destroy/load_rom/run_frame/set_keys/get_frame）");
        unload();
        return false;
    }
    return true;
#else
    m_error = "JZEmulator 目前只在 Windows 下可用（宿主 DLL 是 Windows 二进制）";
    return false;
#endif
}

void JZEmulatorCore::unload()
{
    closeRom();
#ifdef _WIN32
    if (m_module)
    {
        ::FreeLibrary((HMODULE)m_module);
        m_module = nullptr;
    }
#endif
    m_createApi = nullptr;
    m_destroy = nullptr;
    m_loadRom = nullptr;
    m_runFrame = nullptr;
    m_setKeys = nullptr;
    m_getFrame = nullptr;
}

// ==================== ROM ====================
bool JZEmulatorCore::openRom(const std::string& path)
{
    m_error.clear();
    closeRom();

    if (!isLoaded())
    {
        m_error = "还没加载 " + std::string(kDllName) + "（先 load()）";
        return false;
    }
    if (!file_exists(path))
    {
        m_error = "找不到 ROM: " + path;
        return false;
    }

    // 每次开 ROM 都建一个**全新实例**：换 ROM / 重开时不带上一局的状态残留
    m_core = m_createApi();
    if (!m_core)
    {
        m_error = "mgba_create 失败（内存不足？）";
        return false;
    }
    if (m_loadRom(m_core, to_ansi(path).c_str()) != 0)
    {
        m_error = "mgba_load_rom 失败（ROM 格式不认识 / 文件损坏？）: " + path;
        closeRom();
        return false;
    }
    m_keys = 0;
    return true;
}

void JZEmulatorCore::closeRom()
{
    if (m_core && m_destroy)
        m_destroy(m_core);
    m_core = nullptr;
    m_keys = 0;
}

// ==================== 运行 ====================
void JZEmulatorCore::runFrame()
{
    if (m_core && m_runFrame)
        m_runFrame(m_core);
}

void JZEmulatorCore::setKeys(uint32_t keys)
{
    m_keys = keys;
    if (m_core && m_setKeys)
        m_setKeys(m_core, (unsigned int)keys);
}

const uint8_t* JZEmulatorCore::frame(int* w, int* h) const
{
    if (w) *w = 0;
    if (h) *h = 0;
    if (!m_core || !m_getFrame)
        return nullptr;

    int fw = 0, fh = 0;
    m_getFrame(m_core, m_frameBuf.data(), &fw, &fh);
    if (fw <= 0 || fh <= 0)
        return nullptr;
    if (fw > kMaxFrameW || fh > kMaxFrameH)
    {
        // 宿主 DLL 给了超出预期的尺寸：宁可报错也不越界读（缓冲是按上界分的）
        m_error = "帧尺寸超出预期: " + std::to_string(fw) + "x" + std::to_string(fh);
        return nullptr;
    }
    if (w) *w = fw;
    if (h) *h = fh;
    return m_frameBuf.data();
}
