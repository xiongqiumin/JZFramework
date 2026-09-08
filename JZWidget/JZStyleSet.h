#ifndef JZ_STYLE_SET_H_
#define JZ_STYLE_SET_H_

#include "JZColor.h"
#include "JZRect.h"
#include "JZOptional.h"

#include <algorithm>
#include <string>
#include <vector>

// 控件已解析的样式值 (盒子外观)。
// 可选字段为 None 时表示"不绘制该项"; 后续由样式表 (JZStyleSheet) 解析填充,
// 当前由控件在构造时按默认外观设置。
struct JZStyleSet
{
    JZOptional<JZColor> background;    // 背景色 (None/透明 = 不绘制)
    JZOptional<JZColor> borderColor;   // 边框色 (None/透明 = 不绘制)
    JZOptional<JZColor> color;         // 内容层文本色 (None = 用控件自带默认色)
    int borderWidth = 1;
    int radius = 0;                    // 圆角 (px)

    int paddingLeft = 0;
    int paddingTop = 0;
    int paddingRight = 0;
    int paddingBottom = 0;

    // 内容区 = box 内缩 (borderWidth + padding)
    JZRect contentRect(const JZRect& box) const
    {
        int bw = borderColor.isSome() ? borderWidth : 0;
        int left = bw + paddingLeft;
        int top = bw + paddingTop;
        int right = bw + paddingRight;
        int bottom = bw + paddingBottom;
        return JZRect(box.x() + left, box.y() + top,
                      std::max(0, box.width() - left - right),
                      std::max(0, box.height() - top - bottom));
    }

    // ---- 声明应用 (属性值文本 -> 类型化写入) ----
    // 属性名须已小写; 值由 JZStyleSheetParser 产出 (var() 已替换)。
    // 返回是否识别该属性; 未识别的属性静默忽略 (对齐设计: 未知属性接受并忽略)。
    // 支持: background-color / background(色值子集) / color / border(简写与分项) /
    //       border-color / border-width / border-radius / padding(1~4 值) 与四个分项
    bool applyDecl(const std::string& property, const std::string& value);

    // 便捷: 一次设四边边框
    void setBorder(const JZColor& c, int w)
    {
        borderColor = JZOptional<JZColor>::Some(c);
        borderWidth = w;
    }
};

#endif // JZ_STYLE_SET_H_
