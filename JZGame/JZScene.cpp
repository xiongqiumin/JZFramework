#include "JZScene.h"

#include <algorithm>

void JZScene::removeAt(size_t index)
{
    if (index < m_sprites.size())
        m_sprites.erase(m_sprites.begin() + (std::ptrdiff_t)index);
}

void JZScene::clear()
{
    m_sprites.clear();
}

void JZScene::draw(JZPainter* painter) const
{
    if (!painter)
        return;

    m_drawList.clear();
    for (const auto& s : m_sprites) {
        if (s->visible && s->alpha > 0.0)
            m_drawList.push_back(s.get());
    }
    // z 稳定排序: 同 z 保持加入顺序 (后加者画在上层)
    std::stable_sort(m_drawList.begin(), m_drawList.end(),
        [](const JZSprite* a, const JZSprite* b) { return a->z < b->z; });
    for (const JZSprite* s : m_drawList)
        s->draw(painter);
}

void JZScene::drawVisible(JZPainter* painter, const JZBody& view) const
{
    if (!painter)
        return;

    m_drawList.clear();
    for (const auto& s : m_sprites) {
        // 过滤顺序: 便宜的先判 (visible/alpha 是字段; body() 要算一次 AABB)
        if (!s->visible || s->alpha <= 0.0)
            continue;
        if (!s->body().intersects(view))
            continue;
        m_drawList.push_back(s.get());
    }
    std::stable_sort(m_drawList.begin(), m_drawList.end(),
        [](const JZSprite* a, const JZSprite* b) { return a->z < b->z; });
    for (const JZSprite* s : m_drawList)
        s->draw(painter);
}

size_t JZScene::countVisible(const JZBody& view) const
{
    size_t n = 0;
    for (const auto& s : m_sprites) {
        if (!s->visible || s->alpha <= 0.0)
            continue;
        if (s->body().intersects(view))
            ++n;
    }
    return n;
}
