#ifndef JZ_FONT_METRICS_H_
#define JZ_FONT_METRICS_H_

#include <string>
#include "JZFont.h"

// 字体度量后端接口: 由渲染后端实现并在 init 时注册 (如 JZGdiBackend),
// 保证控件测量与最终绘制使用同一套字体。后端销毁前须置空注册。
class JZFontEngine
{
public:
    virtual ~JZFontEngine() = default;
    virtual int textWidth(const std::string& text, int pixelSize) = 0;
    virtual int textHeight(int pixelSize) = 0;
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

private:
    static int estimateWidth(const std::string& text, int pixelSize);
};

#endif // JZ_FONT_METRICS_H_
