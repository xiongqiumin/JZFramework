#include "JZSprite.h"
#include "JZPainter.h"
#include "JZPen.h"

JZSprite::~JZSprite() = default;

void JZSprite::draw(JZPainter* p) const
{
    if (!p || !visible || alpha <= 0.0)
        return;

    double w = width, h = height;
    const bool hasTex = !texture.isNull();
    if (hasTex && (w <= 0 || h <= 0)) {
        w = texture.width();
        h = texture.height();
    }
    if (w <= 0 || h <= 0)
        return;

    // 模板方法: 状态与变换的统一管线, 子类只碰 onDraw
    p->save();
    p->translate(x, y);
    if (rotation != 0.0)
        p->rotate(rotation);
    if (scaleX != 1.0 || scaleY != 1.0)
        p->scale(scaleX, scaleY);
    onDraw(p);
    p->restore();
}

void JZSprite::onDraw(JZPainter* p) const
{
    double w = width, h = height;
    const bool hasTex = !texture.isNull();
    if (hasTex && (w <= 0 || h <= 0)) {
        w = texture.width();
        h = texture.height();
    }
    if (w <= 0 || h <= 0)
        return;

    // 锚点对齐: 把几何左上角从局部原点挪到锚点处 ((0,0)=左上角 与 Rect 语义一致)
    const int ox = (int)(-anchorX * w);
    const int oy = (int)(-anchorY * h);
    const int iw = (int)w;
    const int ih = (int)h;

    if (hasTex) {
        // 纹理路径: 预转 PARGB, 零逐像素开销; 不支持逐次 alpha/染色
        p->drawPixmap(texture, ox, oy, iw, ih);
        return;
    }

    JZColor c = color;
    c.setRgba(c.red(), c.green(), c.blue(), (uint8_t)(c.alpha() * alpha));
    if (shape == Ellipse) {
        p->fillEllipse(ox, oy, iw, ih, c);
        if (borderWidth > 0) {
            JZColor bc = borderColor;
            bc.setRgba(bc.red(), bc.green(), bc.blue(), (uint8_t)(bc.alpha() * alpha));
            p->setPen(JZPen(bc, borderWidth));
            p->drawEllipse(ox, oy, iw, ih);
        }
    } else {
        p->fillRect(ox, oy, iw, ih, c);
        if (borderWidth > 0) {
            JZColor bc = borderColor;
            bc.setRgba(bc.red(), bc.green(), bc.blue(), (uint8_t)(bc.alpha() * alpha));
            p->setPen(JZPen(bc, borderWidth));
            p->drawRect(ox, oy, iw, ih);
        }
    }
}

JZBody JZSprite::body() const
{
    double w = width, h = height;
    if (!texture.isNull() && (w <= 0 || h <= 0)) {
        w = texture.width();
        h = texture.height();
    }
    const double sw = w * scaleX;
    const double sh = h * scaleY;
    return JZBody(x - anchorX * sw, y - anchorY * sh, sw, sh);
}
