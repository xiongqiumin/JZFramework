#ifndef JZ_GAME_VIEW_H_
#define JZ_GAME_VIEW_H_
#pragma once

#include "JZWidget.h"
#include "JZPainter.h"
#include "JZKeyEvent.h"
#include "JZShowEvent.h"
#include "JZTimer.h"
#include "JZFixedTimer.h"

#include <chrono>
#include <cstdint>
#include <set>
#include <string>

// ============================================================================
// JZGameView: 2D 游戏画布控件 (JZGame 模块的基类)。
//
// 定位: **不带主循环引擎** (没有场景图/精灵系统/世界), 只把 test/widget/shooter
// 验证过的"两个 timer"姿势封装成基类:
//   * 逻辑 timer (JZFixedTimer, 固定步长 + 落后补帧): 每个步长调一次 stepGame();
//     仿真速度与渲染帧率解耦 —— 卡顿不会让游戏变慢, 只会补帧;
//   * 渲染 timer (JZTimer, 默认 0 = 尽可能快): 每帧推进动画时钟 + FPS 统计 + update()
//     触发重绘; paintEvent 里按本控件建好 JZPainter 交给 renderGame()。
// 两个 timer 的创建/启停/参数全在基类, 子类只重写钩子, 不碰 timer。
//
// 用法:
//   class MyGame : public JZGameView {
//   protected:
//       void resetGame() override { ... }              // show 时调一次 (width/height 已就绪)
//       void stepGame()  override { ... }              // 固定步长仿真 (输入查 isKeyDown)
//       void renderGame(JZPainter* p) override { ... } // 自绘 (变换用 animTime())
//   };
//
// 参考实现: test/game/shooter (与 test/widget/shooter 同一套玩法, 对照看基类省了什么)。
// ============================================================================
class JZFRAMEWORK_API JZGameView : public JZWidget
{
public:
    explicit JZGameView(JZWidget* parent = nullptr);
    ~JZGameView() override;

    // ---- 两个 timer 的参数 ----
    void setLogicInterval(int ms);      // 逻辑步长 (默认 16ms ~60Hz; 运行中改会重起逻辑 timer)
    int  logicInterval() const;
    void setMaxCatchUp(int frames);     // 单次最多补帧数 (默认 30; 落后更多 -> onFallBehind)
    int  maxCatchUp() const;
    void setRenderInterval(int ms);     // 渲染间隔 (默认 0 = 尽可能快, 用于压 FPS 上限)
    int  renderInterval() const;

    // ---- 逻辑 timer 控制 (落后停止后恢复 / 子类"按 R 重开"用) ----
    void restartLogic();                // 清掉 errorMessage 并按当前步长重起逻辑 timer
    void stopLogic();
    bool logicActive() const;

    // 逻辑落后过多停止后非空 (onFallBehind 的默认实现会设置); restartLogic 清空
    const std::string& errorMessage() const;

    // ---- 键盘 (基类在 keyPress/keyRelease 里维护"当前按住"的虚拟键码集合) ----
    bool isKeyDown(uint32_t vk) const;
    const std::set<uint32_t>& keysDown() const;

    // ---- 动画时钟 (渲染驱动, 与逻辑步长解耦; 驱动旋转/缩放等矩阵变换) ----
    double animTime() const { return m_animT; }
    double animDelta() const { return m_animDt; }

    // ---- 帧率统计 (渲染 timer 实测; 间隔 0 时接近 GDI 实际上限) ----
    float fps() const { return m_fps; }
    float peakFps() const { return m_peakFps; }

protected:
    // ---- 钩子 (子类重写; 基类实现都是空) ----
    virtual void resetGame() {}          // showEvent 里调一次 (此时 width()/height() 已就绪)
    virtual void stepGame() {}           // 每个逻辑步长一次 (落后时可能连续多次补帧)
    virtual void renderGame(JZPainter* p) {}  // paintEvent 内 (painter 已按本控件构造, 裁剪已设)
    virtual void onFallBehind(int missed);    // 逻辑落后超过 maxCatchUp: 默认停 + 记 errorMessage

    // ---- 事件 (基类已处理好 timer 启停与键盘登记, 一般不必重写) ----
    void showEvent(JZShowEvent* event) override;
    void paintEvent(JZPainterEvent* event) override;
    void keyPressEvent(JZKeyEvent* event) override;
    void keyReleaseEvent(JZKeyEvent* event) override;

private:
    void onLogicTick();
    void onRenderTick();
    void onLogicErrorSlot(int missed);   // sigError -> 虚钩子 onFallBehind (避免成员指针直连虚函数)

    JZFixedTimer* m_logicTimer = nullptr;    // 逻辑: 固定步长 + 自动补帧
    JZTimer*      m_renderTimer = nullptr;   // 渲染: 默认 0=尽可能快

    int m_logicMs = 16;        // 逻辑步长 (~60Hz)
    int m_maxCatchUp = 30;     // 单次补帧上限 (窗口创建/首帧那段很容易掉 6~10 帧, 默认 5 会把自己停死)
    int m_renderMs = 0;        // 渲染间隔: 0 = 尽可能快

    std::set<uint32_t> m_keys;   // 当前按下的虚拟键码 (JZKeyEvent::key 即平台 VK_*)
    std::string m_errMsg;        // 非空 = 逻辑落后过多, 游戏已停止

    double m_animT = 0.0;        // 动画时钟 (渲染期推进)
    double m_animDt = 0.0;       // 上一帧时长 (秒)
    float  m_fps = 0.0f;
    float  m_peakFps = 0.0f;
    std::chrono::steady_clock::time_point m_lastFpsTick;
    std::chrono::steady_clock::time_point m_animTick;
};

#endif // !JZ_GAME_VIEW_H_
