#ifndef JZ_GAME_BODY_H_
#define JZ_GAME_BODY_H_
#pragma once

#include "JZGlobal.h"   // JZFRAMEWORK_API

// ============================================================================
// JZBody: AABB 碰撞体 (世界坐标, double)。
//
// "先 AABB": 弹幕游戏的判定绝大多数是"玩家小判定点 vs 一堆子弹的外接框",
// AABB 足够且极便宜 (4 次比较)。需要圆/多边形时在此之上叠加, 不改这个结构。
//
// 坐标约定: (x,y) = 左上角, (w,h) = 尺寸; 与 JZRect 的闭区间语义不同, 这里
// 用 [x, x+w) 半开区间做重叠测试 (浮点世界坐标的标准做法)。
// ============================================================================
class JZFRAMEWORK_API JZBody
{
public:
    JZBody() = default;
    JZBody(double x, double y, double w, double h)
        : x(x), y(y), w(w), h(h) {}

    // 以中心点构造 (弹幕判定常用: 玩家判定点是个居中小编)
    static JZBody fromCenter(double cx, double cy, double w, double h)
    {
        return JZBody(cx - w * 0.5, cy - h * 0.5, w, h);
    }

    bool intersects(const JZBody& o) const
    {
        return x < o.x + o.w && o.x < x + w && y < o.y + o.h && o.y < y + h;
    }

    bool contains(double px, double py) const
    {
        return px >= x && px <= x + w && py >= y && py <= y + h;
    }

    double centerX() const { return x + w * 0.5; }
    double centerY() const { return y + h * 0.5; }

    double x = 0;   // 左上角
    double y = 0;
    double w = 0;   // 尺寸
    double h = 0;
};

#endif // !JZ_GAME_BODY_H_
