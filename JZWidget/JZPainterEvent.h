#ifndef JZ_PAINTER_EVENT_H_
#define JZ_PAINTER_EVENT_H_

#include "JZEvent.h"
#include "JZRect.h"

class JZPainter;

class JZInvalidateEvent : public JZEvent
{
public:
    explicit JZInvalidateEvent();

    JZRect rect;
};

// Created by the render scheduler during the refresh traversal.
// Widgets get the painter via painter() and the paint area via rect()
// (dirty/clip area, for virtual lists/tables to paint visible parts only).
class JZPainterEvent : public JZEvent
{
public:
    explicit JZPainterEvent();

    void setPainter(JZPainter* painter) { m_painter = painter; }
    JZPainter* painter() const { return m_painter; }
    void setRect(const JZRect& rc) { m_rect = rc; }
    JZRect rect() const { return m_rect; }

private:
    JZPainter* m_painter = nullptr;
    JZRect m_rect;
};

#endif