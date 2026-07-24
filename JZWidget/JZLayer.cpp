#include "JZLayer.h"
#include <algorithm>

JZLayer::JZLayer(int w, int h)
    : m_buf(w, h)
{
}

void JZLayer::resize(int w, int h)
{
    m_buf.resize(w, h);
}

void JZLayer::dispatch()
{
    JZRect bufBounds = m_buf.bounds();
    for (const JZDrawTask& t : m_tasks) {
        JZRect a = t.area.intersect(t.clip);
        a = a.intersect(bufBounds);
        if (a.isEmpty())
            continue;

        if (t.type == JZDrawTaskType::Rect) {
            m_buf.fillRect(a.x(), a.y(), a.width(), a.height(), t.color);
        } else if (t.type == JZDrawTaskType::Border) {
            int bw = std::max(1, t.borderWidth);
            int x = a.x(), y = a.y(), w = a.width(), h = a.height();
            m_buf.fillRect(x, y, w, bw, t.color);                                 // top
            m_buf.fillRect(x, y + h - bw, w, bw, t.color);                        // bottom
            m_buf.fillRect(x, y, bw, h, t.color);                                 // left
            m_buf.fillRect(x + w - bw, y, bw, h, t.color);                        // right
        }
    }
}
