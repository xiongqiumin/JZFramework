#ifndef JZ_GAME_VIEW_2D_H_
#define JZ_GAME_VIEW_2D_H_
#pragma once

#include "JZGameGlobal.h"
#include "JZGameView.h"
#include "JZCamera2D.h"

#include <string>

class JZPixmap;

// ============================================================================
// JZGameView2D: 带 2D 滚屏相机的游戏控件 (JZGameView + JZCamera2D)
//
// 它替子类管掉"滚屏"这套固定动作, 子类只写玩法:
//
//   子类钩子                  坐标空间                 典型内容
//   -----------------------  ------------------------  --------------------------
//   stepGame()               世界 (自己算)             输入/物理/AI/相机跟随
//   renderBackground(p)      **屏幕坐标** (相机之前)   天空/视差/平铺地砖
//   renderWorld(p)           **世界坐标** (已施加相机)  地图/精灵/角色
//   renderHud(p)             **屏幕坐标** (相机已还原) 血条/分数/小地图
//
// 三层顺序是固定的: 背景(不吃相机) -> 世界(吃相机) -> HUD(不吃相机)。
// 这样"视差/平铺"天然在角色下面, 不需要子类自己算绘制顺序。
//
// 与 JZGameView 的关系:
//   * 两个 timer、键盘集合、fps、落后停止全部照旧 (从 JZGameView 继承, 不重复);
//   * 只多了一层"paintEvent -> 背景 -> 施加相机 -> renderWorld -> 还原 -> renderHud"。
//   所以从 JZGameView 迁过来只需把 `renderGame` 拆成 renderWorld + renderHud 两个。
//
// 相机是**成员对象**而不是指针: 生命周期跟控件一致, 没有 new/delete。
// 默认视口 = 控件客户区 (resize 自动同步), 默认不设世界边界、followRate=8。
//
// ⚠ 不要在子类里重写 renderGame / paintEvent —— 那会把相机变换绕过去。
//   要"某层不吃相机"就放 renderBackground / renderHud。
//
// 滚屏三件套的配合:
//   camera.setWorldSize(w, h);          // 世界多大 (不设 = 不钳边界)
//   followCamera(px, py);               // stepGame 里每步一次
//   if (isVisible(sprite.body())) ...   // 只更新/只画屏幕内的东西
//   drawTiledBackground(p, tex, 0.3);   // 视差/平铺背景 (在 renderBackground 里调)
// ============================================================================
class JZGAME_API JZGameView2D : public JZGameView
{
public:
    explicit JZGameView2D(JZWidget* parent = nullptr);
    ~JZGameView2D() override = default;

    // ---- 相机 ----
    JZCamera2D& camera() { return m_camera; }
    const JZCamera2D& camera() const { return m_camera; }

    // 关掉后 renderWorld 直接拿到未变换的 painter (想自己接管相机/做分屏时用)
    void setCameraEnabled(bool on) { m_cameraEnabled = on; }
    bool isCameraEnabled() const { return m_cameraEnabled; }

    // stepGame 里常用的简写: 用**逻辑步长**当 dt 调 camera().follow(...)
    // (逻辑 timer 是固定步长, 所以这里的跟随与渲染帧率无关)
    void followCamera(double targetX, double targetY);

    // ---- 剔除 ----
    // 当前视口覆盖的世界矩形 (相机未启用时返回 (0,0,0,0))
    JZBody visibleWorld() const;
    // 世界 AABB 是否需要画/更新 (pad 语义见 JZCamera2D::isVisible)
    bool isVisible(const JZBody& worldBody, double pad = 0.0) const;

    // ---- 视差 / 平铺背景 (在 renderHud 里调, 屏幕坐标) ----
    // 把**已预转好**的 pixmap (JZPixmap::fromImage) 按视口铺满, 并按相机位移滚动:
    //   factorX = 0   钉在屏幕上 (HUD 底图/天空)
    //   factorX = 0.3 远景 (比世界慢 => 视差)
    //   factorX = 1   与地面同速 (贴图世界的"地砖")
    // 只铺可见的那几块 (不会画到视口外), 也不受 zoom 影响 (背景按像素铺)。
    void drawTiledBackground(JZPainter* p, const JZPixmap& tex,
                             double factorX, double factorY);

    // 只铺**一行** (横向平铺, 纵向固定 y): 视差山脊/云带/路面这类"条状背景"用它。
    // 比 drawTiledBackground 省得多 —— 一张 256x200 的图只画 3~4 次, 而不是铺满整屏。
    void drawTiledRow(JZPainter* p, const JZPixmap& tex, double factorX, int y);

protected:
    // 子类重写这三个 (renderGame 由本类实现, 不要再重写)
    virtual void renderBackground(JZPainter* p) { (void)p; }
    virtual void renderWorld(JZPainter* p) { (void)p; }
    virtual void renderHud(JZPainter* p) { (void)p; }

    // renderGame = renderBackground -> 施加相机 -> renderWorld -> 还原 -> renderHud
    void renderGame(JZPainter* p) override;

    void resizeEvent(JZResizeEvent* event) override;

private:
    void syncViewport();

    JZCamera2D m_camera;
    bool m_cameraEnabled = true;
};

#endif // !JZ_GAME_VIEW_2D_H_
