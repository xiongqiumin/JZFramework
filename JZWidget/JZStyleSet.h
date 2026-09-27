#ifndef JZ_STYLE_SET_H_
#define JZ_STYLE_SET_H_
#include "JZColor.h"
#include "JZRect.h"
#include "JZOptional.h"

#include <algorithm>
#include <string>
#include <vector>

// ---- 样式属性词汇表 (唯一真相源) ----
// 解析 (JZStyleSet::applyDecl) 与能力查询 (meta / 校验) 共用这一张表,
// 因此不会出现"实现支持但没登记 / 登记了但实现不支持"的漂移。
// 注意: 词汇表只回答"引擎有没有这个属性、值怎么解析";
//       "某个控件会不会消费它"由该控件自己声明 (JZWidget::styleProps)。
enum class JZFRAMEWORK_API JZStyleValueKind
{
    Color,             // 颜色 (#RRGGBB / rgb() / transparent / 命名色)
    Length,            // 单长度 (Npx)
    LengthBox,         // 1~4 值长度 (padding 简写)
    BorderShorthand,   // <w>px [solid|none] <color> 或 none
    Keyword,           // 关键字 (text-align / font-weight ...)
    Number,            // 数字 (opacity ...)
    String,            // 字符串 (font-family ...)
};

struct JZFRAMEWORK_API JZStylePropInfo
{
    std::string name;
    JZStyleValueKind kind;
    bool implemented;   // false = 已知属性但尚未生效 (写了会被忽略, 登记出来便于提示)
};

// 控件已解析的样式值 (盒子外观)。
// 可选字段为 None 时表示"不绘制该项"; 后续由样式表 (JZStyleSheet) 解析填充,
// 当前由控件在构造时按默认外观设置。
struct JZFRAMEWORK_API JZStyleSet
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

    // ---- 属性词汇表 ----
    static const std::vector<JZStylePropInfo>& propertyTable();
    // 查属性; 未知属性返回 nullptr
    static const JZStylePropInfo* findProperty(const std::string& name);

    // ---- 声明应用 (属性值文本 -> 类型化写入) ----
    // 属性名须已小写; 值由 JZStyleSheetParser 产出 (var() 已替换)。
    // 查词汇表: 未知属性或 implemented=false 的属性一律忽略 (不报错)。
    // 当前生效的: background-color / background(色值子集) / color / border(简写与分项) /
    //             border-color / border-width / border-radius / padding(1~4 值) 与四个分项
    bool applyDecl(const std::string& property, const std::string& value);

    // 便捷: 一次设四边边框
    void setBorder(const JZColor& c, int w)
    {
        borderColor = JZOptional<JZColor>::Some(c);
        borderWidth = w;
    }
};

#endif // JZ_STYLE_SET_H_
