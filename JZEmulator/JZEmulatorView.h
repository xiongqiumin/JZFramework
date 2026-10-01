#ifndef JZ_EMULATORVIEW_H_
#define JZ_EMULATORVIEW_H_

#include <map>
#include <set>
#include <string>
#include <vector>

#include "JZWidget.h"
#include "JZImage.h"

#include "JZEmulatorCore.h"

class JZFixedTimer;
class JZKeyEvent;
class JZPainterEvent;
class JZShowEvent;

// ============================================================================
// JZEmulatorView —— 把 mGBA 的画面贴到控件上，并把键盘喂给模拟器
// ============================================================================
// 定位：虚拟桌面（或真窗口）里的一个"游戏控件"。上层只按键、只看画面：
//   · 人工：宿主窗口里直接玩；
//   · agent/容器：virtual_device 起来后，`GET /api/screen?window=` 拿到的就是它，
//     `POST /api/input/key`（press:true/false 支持按住）打进来的就是它的键盘。
//   于是"看屏幕 -> 决策 -> 按键"那条 computer-use 链路一行都不用改。
//
// 帧驱动：JZFixedTimer 每拍（默认 16ms ≈ 60Hz）①同步按键 ②跑一帧 ③换帧 + update()。
//   ⚠ 用 JZFixedTimer 而不是 JZTimer：框架的定时器建在 Win32 SetTimer/WM_TIMER 上
//     （JZWin32Thread.cpp:99-101 的 30ms heartbeat、:248 的 gap 唤醒），唤醒精度就是系统
//     时钟粒度（~15.6ms）—— 实测 800ms 只跑了 39~42 帧（每帧 ~19~20ms）。JZTimer 是
//     "慢了就慢了"，游戏内时间会跟着变慢；JZFixedTimer 以**墙钟**为基准连续补帧
//     （JZFixedTimer.cpp:92-103：behind / interval + 1），于是节拍抖动被补掉、时间不漂移。
//     防死亡螺旋：单次落后超过 maxCatchUp 就 stop + sigError（见 onTimerError）。
//
// ⚠ 这里**既不做超分、也不做放大**（用户拍定：超分 50ms/帧太慢，放大也不要）：
//   画面就是 1:1 上屏（GBA 原生 240x160），控件比它大就四周留黑边。
//   这么做还有一个副作用是好的：贴图时源/目标尺寸相等 ⇒ 后端走直拷
//   （JZGdiBackend.cpp:166 只有尺寸相等才不重采样），既快又不会糊。
//   真要放大是**外层**的事：无头容器可以用 POST /api/desktop {scale} 整体放大
//   （只影响截图分辨率、不进坐标），不该在控件里做。
// ============================================================================
class JZEmulatorView : public JZWidget
{
public:
    explicit JZEmulatorView(JZWidget* parent = nullptr);
    ~JZEmulatorView();

    // 加载宿主 DLL（dllDir 里找 mgba_host.dll；空 = 默认搜索路径）+ 打开 ROM。
    // 失败原因见 lastError()。
    bool openRom(const std::string& dllDir, const std::string& romPath);
    bool isRomOpen() const { return m_core.isRomOpen(); }
    std::string lastError() const { return m_core.lastError(); }

    void start(int intervalMs = 16);
    void stop();
    bool isRunning() const;

    int frameCount() const { return m_frames; }
    uint32_t keys() const { return m_core.keys(); }

    // 给驱动/测试/截图用：当前帧（BGRA，尺寸经 w/h 回填）。
    // ⚠ 只读，且下一拍就会被覆盖 —— 要留就自己拷一份。
    const uint8_t* frame(int* w, int* h) const { return m_core.frame(w, h); }

protected:
    void paintEvent(JZPainterEvent* event) override;
    void keyPressEvent(JZKeyEvent* event) override;
    void keyReleaseEvent(JZKeyEvent* event) override;
    void showEvent(JZShowEvent* event) override;

private:
    void onTick();
    // JZFixedTimer 落后过多（补不回来）时的上报槽；它已经自己 stop 了，这里只记一条日志
    void onTimerError(int missed);
    // 把"当前按下的键"同步给模拟器。**控件没有焦点时一律松手** ——
    // 否则点到别处/窗口失活后会留下一个卡住的键（游戏里表现为一直往一个方向走）。
    void syncKeys();
    static uint32_t gbaKeyOf(int vk);

    JZEmulatorCore m_core;
    JZFixedTimer* m_timer = nullptr;
    JZImage m_image;               // 当前显示帧（每拍整体换新，见 .cpp 的缓存说明）
    std::vector<uint8_t> m_rgba;   // 转换缓冲（BGRA(X) -> RGBA8888）
    std::set<int> m_pressed;       // 当前按下的 VK 键码
    // "至少压住一帧"（见 .cpp 的 keyReleaseEvent）：
    // 程序注入的按键是 press+release 一瞬间，两次之间若没跑过一帧，那一按就白按了。
    std::map<int, int> m_pressFrame;   // vk -> 按下时的帧号
    std::set<int> m_lateRelease;       // 下一帧跑完才真松开的键
    int m_frames = 0;
};

#endif // JZ_EMULATORVIEW_H_
