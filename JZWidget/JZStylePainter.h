#ifndef JZ_STYLE_PAINTER_H_
#define JZ_STYLE_PAINTER_H_

#include "JZPainter.h"
#include "JZStyleSet.h"

// 样式盒子绘制器: 控件在 paintEvent 内主动调用, 把样式中的背景/边框画到自己的矩形上。
// 绘制流裁剪 (脏区 ∩ 控件 rect) 由 JZPainter 保证, 样式不会画出控件范围;
// 控件 rect 之外 (margin 区) 不属于绘制范围, 露出父控件背景。
class JZStylePainter
{
public:
    // 绘制样式盒子: 背景 (填满 box, 支持圆角) + 边框 (贴 box 边缘)。
    static void drawBox(JZPainter& p, const JZStyleSet& style, const JZRect& box);

    // 内容区 = box 内缩 (borderWidth + padding)
    static JZRect contentRect(const JZStyleSet& style, const JZRect& box)
    {
        return style.contentRect(box);
    }
};

#endif // JZ_STYLE_PAINTER_H_
