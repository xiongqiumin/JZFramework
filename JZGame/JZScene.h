#ifndef JZ_GAME_SCENE_H_
#define JZ_GAME_SCENE_H_
#pragma once

#include "JZGlobal.h"   // JZFRAMEWORK_API
#include "JZSprite.h"
#include "JZBody.h"
#include "JZPainter.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

// ============================================================================
// JZScene: 精灵容器 + 批量绘制 + AABB 查询。
//
// 定位: **不持有游戏状态** (没有 update、没有父子变换、没有消息)。它就是一块
// "这一帧要画什么"的绘制队列 —— scene.draw 就是那个"渲染系统"(ECS 意义上的
// 系统遍历: 可见性过滤 -> z 稳定排序 -> 逐个虚分发), 外加弹幕最常用的查询:
// 谁和这个 AABB 重叠了。典型分层用法见 test/game/danmaku (敌弹/玩家弹/角色/特效)。
//
// 存储: std::unique_ptr<JZSprite>。因为 JZSprite 可继承 (虚 onDraw), 按值存
// vector<JZSprite> 会**切片**派生状态, 所以走指针:
//   * create<T>(...)  原地构造 (T 为派生类), 弹幕每发一弹一次 create;
//   * adopt(ptr)      收养外部创建的 (对象池复用走这条)。
// 指针的缓存未中在 GDI 后端 (每图元一次调用, μs 级) 下可忽略; 若将来换 GPU
// 批量后端, 再把 Transform/SpriteRender 拆成连续组件做真 ECS (接口已按
// "系统遍历"方向留好: forEach / removeIf / forEachOverlap)。
//
// ⚠ 回调 (forEach/forEachOverlap/removeIf 谓词) 里**不要增删精灵** ——
//   需要删除先打标记 (如 alpha=0), 循环结束后再 removeIf 一次。
// ⚠ create/adopt 之后, 之前取得的指针/引用可能被扩容失效, 不要跨次持有。
// ============================================================================
class JZFRAMEWORK_API JZScene
{
public:
    JZScene() = default;

    // ---- 精灵管理 ----
    // 原地构造一个精灵 (T 必须是 JZSprite 的派生类; 默认 JZSprite 本身)。
    template <class T = JZSprite, class... A>
    T& create(A&&... a)
    {
        m_sprites.emplace_back(new T(std::forward<A>(a)...));
        return static_cast<T&>(*m_sprites.back());
    }

    // 收养外部创建的精灵 (对象池复用走这条); 空指针忽略。
    void adopt(std::unique_ptr<JZSprite> s)
    {
        if (s)
            m_sprites.push_back(std::move(s));
    }

    void removeAt(size_t index);
    // 一趟压缩删除 (谓词: bool(const JZSprite&)); 弹幕清场的主力
    template <class Pred>
    void removeIf(Pred pred)
    {
        m_sprites.erase(std::remove_if(m_sprites.begin(), m_sprites.end(),
            [&pred](const std::unique_ptr<JZSprite>& s) { return pred(*s); }),
            m_sprites.end());
    }
    void clear();

    size_t size() const { return m_sprites.size(); }
    bool empty() const { return m_sprites.empty(); }
    JZSprite* at(size_t i) { return m_sprites[i].get(); }
    const JZSprite* at(size_t i) const { return m_sprites[i].get(); }
    JZSprite* operator[](size_t i) { return m_sprites[i].get(); }
    const JZSprite* operator[](size_t i) const { return m_sprites[i].get(); }

    // ---- 遍历 (逻辑更新用) ----
    template <class F>
    void forEach(F&& f) { for (auto& s : m_sprites) f(*s); }
    template <class F>
    void forEach(F&& f) const { for (const auto& s : m_sprites) f(*s); }

    // ---- 绘制: 可见性过滤 -> z 稳定排序 (同 z 保持加入顺序) -> 逐个 draw ----
    void draw(JZPainter* painter) const;

    // ---- 碰撞查询: 对每个与 body 重叠的精灵调 f(sprite) ----
    // 弹幕热路径 (玩家小判定点 vs 几百发子弹) 就是它; O(n) 但每次只 4 次比较。
    template <class F>
    void forEachOverlap(const JZBody& body, F&& f)
    {
        for (auto& s : m_sprites) {
            if (s->body().intersects(body))
                f(*s);
        }
    }

private:
    std::vector<std::unique_ptr<JZSprite>> m_sprites;
    // draw 的复用缓冲 (stable_sort 的就地目标): 避免每帧分配;
    // 指向本场景精灵, 仅在 draw 内存续期间有效。
    mutable std::vector<const JZSprite*> m_drawList;
};

#endif // !JZ_GAME_SCENE_H_
