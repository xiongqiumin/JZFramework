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

    bool file_exists(const std::string& path)
    {
#ifdef _WIN32
        return !path.empty() && ::GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
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
        std::string d = dir;
        while (!d.empty() && (d[d.size() - 1] == '\\' || d[d.size() - 1] == '/'))
            d.erase(d.size() - 1);
        path = d + "/" + kDllName;
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
    if (m_loadRom(m_core, path.c_str()) != 0)
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
