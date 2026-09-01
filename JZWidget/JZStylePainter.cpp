#include "JZStylePainter.h"

void JZStylePainter::drawBox(JZPainter& p, const JZStyleSet& style, const JZRect& box)
{
    bool hasBg = style.background.isSome() && style.background.value().alpha() > 0;
    bool hasBorder = style.borderColor.isSome() && style.borderColor.value().alpha() > 0
                     && style.borderWidth > 0;
    if (!hasBg && !hasBorder)
        return;

    // 缺失的一侧用全透明代替 (画出来不可见), 复用 drawRoundRect 的填充+描边组合
    JZBrush bgBrush = hasBg ? JZBrush(style.background.value())
                            : JZBrush(JZColor(0, 0, 0, 0));
    JZPen borderPen = hasBorder
        ? JZPen(style.borderColor.value(), style.borderWidth)
        : JZPen(JZColor(0, 0, 0, 0), 1);

    if (style.radius > 0) {
        p.setBrush(bgBrush);
        p.setPen(borderPen);
        p.drawRoundRect(box.x(), box.y(), box.width(), box.height(), style.radius);
        return;
    }

    if (hasBg)
        p.fillRect(box, style.background.value());
    if (hasBorder) {
        p.setPen(borderPen);
        p.drawRect(box.x(), box.y(), box.width(), box.height());
    }
}
