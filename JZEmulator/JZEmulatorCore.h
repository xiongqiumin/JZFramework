#ifndef JZ_EMULATORCORE_H_
#define JZ_EMULATORCORE_H_

#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// JZEmulatorCore —— mGBA 模拟器核心的宿主封装（动态加载 mgba_host.dll）
// ============================================================================
// 模拟器来自 E:/work/simulator 那套工程：mGBA 源码用 **MinGW-w64 (gcc 8.1)** 编成
// libmgba.a，再由它的 host/mgba_host.c 封成一个 **C ABI 的 mgba_host.dll**。
// 本框架是 MSVC，于是两条路：
//   (a) 用 MSVC 重编 mGBA —— 要处理它那份 52KB 的 CMake 与编译器差异，成本高；
//   (b) 跨 **C ABI** 调那个 DLL —— 接口全是 POD（指针/int/字节缓冲），跨编译器安全，
//       不改对方工程，也不用把 libstdc++ 引进本 exe。
// 选 (b)。实测（2026-10-01，VS2015 v140 x64 + MinGW gcc 8.1 编的 DLL）：能加载、
// 能取帧、按键真的进得去模拟器（按住右方向键 60 帧 → 帧差 35.9%）。
//
// ⚠ 那个 DLL 依赖三个 MinGW 运行库（libgcc_s_seh-1 / libstdc++-6 / libwinpthread-1），
//   靠 LoadLibraryEx 的 LOAD_WITH_ALTERED_SEARCH_PATH 从 **DLL 自己所在目录**找 ——
//   所以那三个 DLL 必须和 mgba_host.dll 放在一起（不必进 PATH）。见 load()。
//
// 用法：
//   JZEmulatorCore core;
//   if (!core.load(dllDir))        -> core.lastError()
//   if (!core.openRom(romPath))    -> core.lastError()
//   core.setKeys(JZEmulatorCore::KeyLeft | JZEmulatorCore::KeyA);
//   core.runFrame();
//   int w, h; const uint8_t* bgra = core.frame(&w, &h);   // 240x160
//
// 失败一律给出可读原因（找不到 DLL / 缺导出符号 / ROM 打不开），不静默降级。
// ============================================================================
class JZEmulatorCore
{
public:
    // GBA 键位（与 host/mgba_host.h 的 MGBA_KEY_* 逐位一致）
    enum Key
    {
        KeyA      = 0x001,
        KeyB      = 0x002,
        KeySelect = 0x004,
        KeyStart  = 0x008,
        KeyRight  = 0x010,
        KeyLeft   = 0x020,
        KeyUp     = 0x040,
        KeyDown   = 0x080,
        KeyR      = 0x100,
        KeyL      = 0x200
    };

    JZEmulatorCore();
    ~JZEmulatorCore();
    JZEmulatorCore(const JZEmulatorCore&) = delete;
    JZEmulatorCore& operator=(const JZEmulatorCore&) = delete;

    // 加载 mgba_host.dll。dir 为空 = 按默认搜索路径找；给了 dir 就从该目录加载，
    // 并让它的依赖库也从同一目录找（LOAD_WITH_ALTERED_SEARCH_PATH）。
    bool load(const std::string& dir = std::string());
    void unload();
    bool isLoaded() const { return m_createApi != nullptr; }

    // 打开 ROM（.gba；mGBA 也能认 .gb/.gbc）。同一条路径可重复调用（先关旧的）。
    bool openRom(const std::string& path);
    bool isRomOpen() const { return m_core != nullptr; }
    void closeRom();

    // 跑一帧（约 16.7ms 游戏时间）。没开 ROM 时是空操作。
    void runFrame();

    // 当前按下的键（位或 Key；0 = 全松开）。值不变时不重复下发。
    void setKeys(uint32_t keys);
    uint32_t keys() const { return m_keys; }

    // 当前帧：宽高经 w/h 回填（GBA 是 240x160），像素每 4 字节 **BGRA**
    // （与 GDI+ 32bppARGB 的内存布局一致）。缓冲归本对象所有，
    // 下一次 runFrame() 会覆盖它 —— 要留就自己拷。
    const uint8_t* frame(int* w, int* h) const;

    const std::string& lastError() const { return m_error; }

private:
    // mgba_host.dll 的导出签名（见 E:/work/simulator/host/mgba_host.h）。
    // 这里用 typedef 而不是把 windows.h 拉进头文件（调用方不需要知道 HMODULE）。
    typedef void* (*FnCreate)();
    typedef void  (*FnDestroy)(void*);
    typedef int   (*FnLoadRom)(void*, const char*);
    typedef void  (*FnRunFrame)(void*);
    typedef void  (*FnSetKeys)(void*, unsigned int);
    typedef void  (*FnGetFrame)(void*, unsigned char*, int*, int*);

    void* m_module = nullptr;          // HMODULE（DLL）
    void* m_core = nullptr;            // mgba_host_t*

    FnCreate  m_createApi = nullptr;
    FnDestroy m_destroy = nullptr;
    FnLoadRom m_loadRom = nullptr;
    FnRunFrame m_runFrame = nullptr;
    FnSetKeys m_setKeys = nullptr;
    FnGetFrame m_getFrame = nullptr;

    // 帧缓冲(按 240x160x4 上界分一次, 之后不再分配); frame() 是 const 但要往里写 => mutable
    mutable std::vector<uint8_t> m_frameBuf;
    // 同上: frame() 是 const, 但"尺寸超预期"这类错误要记在对象上给 lastError() 看
    mutable std::string m_error;
    uint32_t m_keys = 0;
};

#endif // JZ_EMULATORCORE_H_
