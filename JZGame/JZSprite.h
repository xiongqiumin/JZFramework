#ifndef JZ_GAME_SPRITE_H_
#define JZ_GAME_SPRITE_H_
#pragma once

#include "JZGlobal.h"   // JZFRAMEWORK_API
#include "JZBody.h"
#include "JZColor.h"
#include "JZPixmap.h"

class JZPainter;

// ============================================================================
// JZSprite: 可继承的轻量精灵 (多态基类)。
//
// 2026-10-12: 从"纯值语义"改为"可继承"—— 用户要子类化定制绘制。取舍:
//   * **draw() 非虚 (模板方法)**: save -> translate/rotate/scale -> onDraw -> restore
//     全在基类, 子类只重写 onDraw 换外观, 不可能弄坏 save/restore 平衡或变换顺序。
//   * **onDraw() 虚钩子**: 默认实现 = 纹理(drawPixmap) / 程序化 Rect/Ellipse;
//     子类重写即整体替换 (进入时局部原点已落在**锚点处**, 见 draw 注释)。
//   * 代价: 多态基类不能按值存进 vector<JZSprite> (派生状态会被切片), 所以
//     JZScene 持 unique_ptr。900 个指针的缓存未中在 GDI 后端 (每图元一次调用,
//     μs 级) 下可忽略; 将来上 GPU 批量后端再拆连续组件 (真 ECS) 不迟。
//
// 弹幕向设计 (不变的部分):
//   * 纹理按值持 JZPixmap (内部 shared_ptr): 全弹幕共享一张图 = 引用计数 +1,
//     drawPixmap 走预转 PARGB, 每发子弹零逐像素开销。
//   * 锚点 (anchorX/anchorY, 0..1): 旋转/缩放绕锚点, (0.5,0.5)=中心。
//   * vx/vy 由玩法层自管: 引擎不积分、不解释。
//   * alpha 仅程序化图形路径生效 (drawPixmap 无逐次 alpha)。
// ============================================================================
class JZFRAMEWORK_API JZSprite
{
public:
    JZSprite() = default;
    virtual ~JZSprite();   // 多态基类: 虚析构 (派生类经 unique_ptr<JZSprite> 删除)

    // ---- 变换 ----
    double x = 0;          // 世界坐标 (锚点所在位置)
    double y = 0;
    double rotation = 0;   // 度, 屏幕顺时针为正
    double scaleX = 1;     // 缩放 (绕锚点)
    double scaleY = 1;
    double anchorX = 0.5;  // 锚点 0..1 (0.5,0.5 = 中心; 0,0 = 左上角)
    double anchorY = 0.5;

    // ---- 尺寸 (逻辑单位) ----
    // width/height == 0 且设了纹理 => 取纹理原始尺寸; 否则按本值。
    double width = 16;
    double height = 16;

    // ---- 外观 ----
    JZPixmap texture;                // 空 = 程序化图形; 非空 = 贴图 (默认 onDraw 里优先)
    enum Shape {
        Rect,      // 矩形 (fillRect)
        Ellipse    // 椭圆/圆 (fillEllipse, w==h 即圆 —— 弹幕玉的主流形状)
    };
    Shape shape = Rect;
    JZColor color = JZColor::White;        // 程序化图形的填充色
    JZColor borderColor = JZColor::Black;  // 描边色 (borderWidth > 0 时生效)
    int borderWidth = 0;             // 0 = 不描边
    double alpha = 1.0;              // 0..1 (仅程序化图形生效)
    bool visible = true;
    int z = 0;                       // 绘制顺序 (大者后画 = 在上层; 同 z 保持加入顺序)

    // ---- 运动 (玩法层自管, 引擎不解释/不积分) ----
    double vx = 0;
    double vy = 0;

    // 绘制到 painter (通常由 JZScene::draw 批量调用; 也可单独画)。
    // **非虚**: 统一做 save/变换/restore 后调虚钩子 onDraw (模板方法模式)。
    void draw(JZPainter* painter) const;

    // 世界坐标 AABB (不计旋转的外接框; 旋转 45° 的方形请自行放大或换碰撞体)。
    JZBody body() const;

    // 以精灵当前位置为中心的碰撞体 (玩家判定点: 比贴图小一圈)
    JZBody centeredBody(double w, double h) const
    {
        return JZBody::fromCenter(x, y, w, h);
    }

protected:
    // 外观钩子 (虚): 默认实现 = 纹理/程序化图形 (见 .cpp)。
    // 进入时 painter 已完成 translate(x,y) -> rotate(rotation) -> scale(sx,sy),
    // **局部原点落在锚点处**: 默认实现按 anchorX/Y 把几何左上角对齐到锚点;
    // 子类若用中心锚 (0.5,0.5), 直接以 (0,0) 为中心画即可。
    // 子类可继续 save/restore 叠加自己的变换 (如自转), 不必管外层状态。
    virtual void onDraw(JZPainter* painter) const;
};

#endif // !JZ_GAME_SPRITE_H_
