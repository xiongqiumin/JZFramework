#ifndef JZ_FONT_METRICS_H_
#define JZ_FONT_METRICS_H_

#include <string>
#include "JZFont.h"

// 字体度量后端接口: 由渲染后端实现并在 init 时注册 (如 JZGdiBackend),
// 保证控件测量与最终绘制使用同一套字体。后端销毁前须置空注册。
// 度量方法携带完整 JZFont (字体族 + 字号 + 风格), 由后端决定如何建 GDI+ 字体。
class JZFontEngine
{
public:
    virtual ~JZFontEngine() = default;
    virtual int textWidth(const std::string& text, const JZFont& font) = 0;
    virtual int textHeight(const JZFont& font) = 0;

    // ---- 文本控件 (光标定位 / 垂直居中) 所需 ----
    // 带默认实现: 后端不实现也能工作, 只是垂直居中会有 1~2px 偏差。
    // 精确实现参考 JZGdiBackend::GdiFontEngine (GDI+ FontFamily::GetCellAscent 等)。
    // 字形 cell 度量只依赖字体族 + 字号, 与风格 (粗体/斜体等) 无关。
    //   ascent:          基线以上高度
    //   descent:         基线以下高度
    //   internalLeading: 字体内部行距 (位于字符 cell 顶部, 字形之上的空白)
    virtual int ascent(const JZFont& font)          { return textHeight(font) * 4 / 5; }
    virtual int descent(const JZFont& font)         { return textHeight(font) - ascent(font); }
    virtual int internalLeading(const JZFont& font) { return 0; }
};

// 字体度量门面 (静态): 控件在 paintEvent / sizeHint 中测量文本,
// 用于居中/对齐, 替代 "字符数 * 7" 式估算。
// 未注册引擎时用 UTF-8 字符宽度估算兜底 (ASCII ~0.55em, CJK ~1em)。
class JZFontMetrics
{
public:
    // 注册平台实现 (不取得所有权)
    static void setEngine(JZFontEngine* engine);

    static int textWidth(const std::string& text, const JZFont& font);
    static int textHeight(const JZFont& font);

    // 便捷: 在 width/height 区域内水平/垂直居中的起点坐标
    static int centerX(const std::string& text, const JZFont& font, int width);
    static int centerY(const JZFont& font, int height);

    // ---- 文本控件专用 ----

    // 前 charCount 个**码点**的宽度 (光标定位必备)。
    // 注意按码点计数而非字节: 直接按字节截断会切断 UTF-8 序列。
    static int textWidth(const std::string& text, const JZFont& font, int charCount);

    static int ascent(const JZFont& font);
    static int descent(const JZFont& font);
    static int internalLeading(const JZFont& font);

    // 单行文本在 height 内垂直居中时, 绘制起始 y (字符 cell 顶部)。
    // 必须扣掉 internalLeading, 否则字形会比视觉中心低 1~2px (中文尤其明显)。
    static int centeredTextTop(const JZFont& font, int height);

    // 字形"可视矩形"高度 = textHeight - internalLeading, 用于画选区/下划线底色
    static int visualHeight(const JZFont& font);

private:
    static int estimateWidth(const std::string& text, int pixelSize);
};

#endif
