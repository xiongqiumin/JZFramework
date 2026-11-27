#ifndef JZ_GAME_CAMERA2D_H_
#define JZ_GAME_CAMERA2D_H_
#pragma once

#include "JZGameGlobal.h"
#include "JZBody.h"

#include <algorithm>
#include <cmath>

class JZPainter;

// ============================================================================
// JZCamera2D: 2D 滚屏相机 (世界坐标 -> 屏幕坐标)
//
// 定位: **只做一件数学事** —— 决定"世界的哪一块显示在视口里"。
//   * 自己没有控件、没有定时器、不持有绘制队列: 由 JZGameView2D 持有并喂视口尺寸；
//     想自己写循环也行 (applyTo(painter) 一句就够)。
//   * 世界坐标与屏幕坐标同向 (y 向下), 单位都是逻辑像素; zoom 是唯一的缩放。
//
// 变换定义 (与 applyTo 完全一致):
//      屏幕 = (世界 - 相机中心) * zoom + 视口中心
// 于是 zoom=1、相机在 (cx,cy) 时, 世界点 (cx,cy) 落在视口正中。
//
// 滚屏的三件事都在这里:
//   1. **跟随**: follow(target, dt) 按指数收敛逼近目标 (与帧率无关);
//      followRate<=0 退化为硬跟随 (瞬移)。
//   2. **边界钳制**: setWorldBounds/setWorldSize 后, 视口永远不会露出世界外面
//      (世界比视口小的那个轴自动居中); SetWorldBounds 之外的 move/setPosition 都会被钳。
//   3. **剔除**: visibleRect() 给出世界坐标的可见矩形, isVisible(body) 判断一个
//      AABB 是否需要画 —— 滚屏地图上绝大多数的精灵都靠它跳过。
//
// 用法 (通常不用直接碰它, JZGameView2D 已代管):
//   camera.setViewportSize(width(), height());
//   camera.setWorldSize(3200, 600);
//   camera.follow(playerX, playerY, 0.016);   // 每个逻辑步
//   ...
//   p->save(); camera.applyTo(p); 画世界; p->restore(); 画 HUD;
// ============================================================================
class JZGAME_API JZCamera2D
{
public:
    JZCamera2D() = default;

    // ---- 视口 (屏幕上的显示区域, 逻辑像素; 一般 = 控件客户区) ----
    void setViewportSize(double w, double h);   // 负值按 0 处理
    double viewportWidth() const { return m_viewW; }
    double viewportHeight() const { return m_viewH; }

    // ---- 位置: 世界坐标里**视口中心**对应的点 ----
    void setPosition(double x, double y);       // 立即定位 (会做边界钳制)
    void move(double dx, double dy);            // 相对移动 (会做边界钳制)
    double x() const { return m_x; }
    double y() const { return m_y; }

    // 视口左/上边在世界坐标里的位置 (= x - viewportWidth/(2*zoom))
    double left() const;
    double top() const;

    // ---- 缩放: 1 = 世界单位=屏幕像素; 0.5 = 看到两倍大的范围 ----
    void setZoom(double z);                     // 钳到 [0.05, 20] 并重算边界
    double zoom() const { return m_zoom; }

    // ---- 世界边界 (可选; 设了才钳制) ----
    // 世界矩形可以不是原点起始 (如 (-200,-100, 4000, 800))。
    void setWorldBounds(const JZBody& bounds);  // w/h<=0 等价 clearWorldBounds()
    void setWorldSize(double w, double h);      // 简写: 世界 = (0,0,w,h)
    void clearWorldBounds();
    bool hasWorldBounds() const { return m_hasBounds; }
    const JZBody& worldBounds() const { return m_bounds; }

    // ---- 平滑跟随 ----
    // rate: 每秒收敛速度 (推荐 6~12); 越大越"跟得紧", <=0 = 硬跟随。
    // 收敛公式 alpha = 1 - exp(-rate*dt) —— 与逻辑步长/渲染帧率都无关。
    void setFollowRate(double perSecond);
    double followRate() const { return m_followRate; }
    void follow(double targetX, double targetY, double dtSeconds);
    void snapTo(double targetX, double targetY);   // 等价 setPosition (语义更直观)

    // ---- 坐标换算 (zoom 已计入) ----
    void worldToScreen(double wx, double wy, double& sx, double& sy) const;
    void screenToWorld(double sx, double sy, double& wx, double& wy) const;
    double worldToScreenX(double wx) const;
    double worldToScreenY(double wy) const;
    double screenToWorldX(double sx) const;
    double screenToWorldY(double sy) const;

    // ---- 可见区 / 剔除 ----
    // 视口覆盖的世界矩形 (JZBody: 左上角 + 尺寸; 视口尺寸为 0 时返回零尺寸)
    JZBody visibleRect() const;
    // 世界 AABB 是否与可见区相交; pad 把可见区四边各外扩 pad 个世界单位
    // (大贴图/拖尾特效的包围盒比实际小的时候给它留余量)
    bool isVisible(const JZBody& worldBody, double pad = 0.0) const;

    // ---- 施加到画笔: 之后所有绘制都用世界坐标 ----
    // 只做 translate/scale, 不改 clip(裁剪仍由控件/脏区决定)。
    void applyTo(JZPainter* painter) const;

    // 按当前 zoom/边界重新钳制相机中心 (setZoom/setViewportSize 内部已调)
    void clamp();

private:
    double m_x = 0.0;          // 相机中心 (世界坐标)
    double m_y = 0.0;
    double m_viewW = 0.0;      // 视口尺寸 (逻辑像素)
    double m_viewH = 0.0;
    double m_zoom = 1.0;
    double m_followRate = 8.0; // 每秒收敛速度
    bool   m_hasBounds = false;
    JZBody m_bounds;
};

#endif // !JZ_GAME_CAMERA2D_H_
