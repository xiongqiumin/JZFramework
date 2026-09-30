#ifndef JZ_PAINTER_H_
#define JZ_PAINTER_H_
#include "JZPen.h"
#include "JZBrush.h"
#include "JZFont.h"
#include "JZColor.h"
#include "JZImage.h"
#include "JZLayer.h"
#include "JZRect.h"
#include "JZMatrix2D.h"
#include "JZPainterPath.h"

#include <vector>

class JZWidget;
class JZDisplayDevice;

// 绘制入口: 在 paintEvent 中由 JZDisplayDevice 创建并下发。
// 所有绘制调用不直接写屏, 而是向当前图层追加 JZDrawTask (等价 LVGL 在 DRAW 事件回调里 lv_draw_xxx)。
class JZFRAMEWORK_API JZPainter
{
public:
    explicit JZPainter(JZWidget* widget);
    // 离屏绘制: 把绘制命令合成到 image 自身的像素缓冲 (坐标即 image 像素, 原点 0,0)。
    // 常用于生成图标/缩略图/离屏画布。image 必须已非 null 且尺寸有效 (见 JZImage::create)。
    // painter 析构时自动把本帧绘制回写入 image (保留原图内容, SourceOver 叠加)。
    // scale: 逻辑单位 -> image 像素的缩放 (默认 1 = "逻辑单位即像素", 与窗口那条路的
    //   JZDisplayDevice::scale 同语义, 只是目标从窗口换成了一张 JZImage)。按宿主 DPI 渲染的
    //   离屏画布 (如虚拟桌面的合成图) 传它, 于是 image 尺寸 = 逻辑尺寸 * scale, 画的时候
    //   仍用逻辑坐标 —— 笔宽/文本/位图都随之按物理分辨率渲染 (文字不糊)。
    explicit JZPainter(JZImage& image, float scale = 1.0f);
    // 离屏**设备**绘制: 把绘制命令交给某个离屏显示设备 —— 它自己的帧缓冲 (后端原生
    // 布局, 如 GDI+ 的 DIB) 就是画布, 析构时一次性回放 (JZDisplayDevice::renderLayer)。
    // 与 JZImage 那条路的区别: 目标布局与后端一致, 每帧不再有 RGBA<->PARGB 全图往返。
    //   - 坐标是**逻辑**的 (与窗口那条路同构), 由设备 DPI 缩放换算到物理像素;
    //   - canvas 为空表示"整块画布"(调用方通常传逻辑尺寸)。
    explicit JZPainter(JZDisplayDevice* device, const JZRect& canvas);
    ~JZPainter();   // 发 Pop 平衡构造时的 Push (未配对的 save 一并自动闭合)

    void endDevice();

    void setPen(const JZPen& pen);
    JZPen pen() const;

    void setBrush(const JZBrush& brush);
    JZBrush brush() const;

    void setFont(const JZFont& font);
    JZFont font() const;

    // 保存/恢复绘制状态 (pen/brush/font/变换矩阵)。save 向绘制流追加 Push, restore 追加 Pop,
    // 与构造/析构的 Push/Pop 共同保证绘制流状态机严格配对。
    void save();
    void restore();

    // ---- 变换矩阵 (控件本地坐标系内, 等价 Qt QPainter 的 worldTransform) ----
    // 组合语义: 每次 translate/rotate/scale 在"当前已变换的坐标系"内继续变换
    // (先 translate 到中心再 rotate => 绕中心旋转)。所有 drawXxx 几何都会先过本矩阵。
    // 实现: 矩阵**不烘焙进坐标**, 而是随 Push/Change 任务下发, 由后端用 GDI+ SetTransform
    // 统一施加 (几何以本地坐标提交)。这样笔宽/文本/图片/圆角都会随变换正确缩放与旋转,
    // 不再有"矩阵缩放不缩放描边宽度"等限定。
    void setTransform(const JZMatrix2D& m);
    JZMatrix2D transform() const;
    void resetTransform();
    void translate(double dx, double dy);
    void rotate(double deg);          // 屏幕顺时针为正, 角度制
    void scale(double sx, double sy);

    // 裁剪属于绘制流状态, 变更时发 Push。
    // setClip(rc): rc 为**控件本地坐标**, 与 drawXxx/fillRect 等各接口一致,
    // paintEvent 内部一律用这个; 内部自动平移到显示(窗口)坐标再写入绘制流。
    // 初始裁剪 (脏区 ∩ 控件全局 rect) 在构造时已设定, 一般无需再调。
    void setClip(const JZRect& clip);

    void drawLine(int x1, int y1, int x2, int y2);
    // 描边矩形/圆角矩形 (用当前 pen), 语义对齐 Qt QPainter::drawRect:
    // 边框恰好包住与 fillRect(x,y,w,h) 相同的像素矩形外圈 [x..x+w-1]×[y..y+h-1],
    // 线条整体落在矩形内 (几何按半笔宽内缩), 因此不会越过控件 clip 造成右/下缺边。
    void drawRect(int x, int y, int w, int h);
    void drawRoundRect(int x, int y, int w, int h, int radius);

    // 填充矩形 (对齐 Qt QPainter::fillRect): 填充像素 [x..x+w-1]×[y..y+h-1],
    // 与 JZRect 闭区间语义 (right=x+w-1) 一致。同参数下 drawRect 描其外圈边框。
    void fillRect(const JZRect &rc, const JZColor& color);
    void fillRect(int x, int y, int w, int h, const JZColor& color);

    // 填充圆角矩形 (只填充不描边, 同参数下 drawRoundRect 还会按当前 brush 填内部)。
    // 真圆 = 正方形且 radius == 边长/2 (如单选钮内点)。
    void fillRoundRect(int x, int y, int w, int h, int radius, const JZColor& color);

    // 角度单位: 1/10 度 (与 LVGL 一致: 0=3点方向, 90=6点方向, 顺时针递增)
    void drawArc(int x, int y, int radius, int startAngle, int endAngle);
    void drawText(int x, int y, const char* text);

    void drawImage(const JZImage& img, int x, int y, int w, int h);
    // 贴显示用位图 (JZPixmap): 同一张图反复绘制时用它 —— 转换已在 JZPixmap::fromImage
    // 时做完, 每次绘制不再有任何逐像素开销 (drawImage 路径会做同样的转换并缓存)。
    void drawPixmap(const JZPixmap& pixmap, int x, int y, int w, int h);

    // ---- 矢量路径 / 曲线 (坐标均为控件本地坐标, 会经过当前变换矩阵) ----
    // 描边 (用当前 pen) / 填充 (用当前 brush; 若 fillColor 指定则用它)。
    void drawPath(const JZPainterPath& path);   // 描边
    void fillPath(const JZPainterPath& path);   // 填充

    // 椭圆 (圆为 w==h): 描边用当前 pen。
    void drawEllipse(int x, int y, int w, int h);
    void fillEllipse(int x, int y, int w, int h, const JZColor& color);

    // 经过 points 的平滑曲线 (Catmull-Rom 展开为三次贝塞尔, 等价 Qt 的 drawPolyline
    // 平滑版)。至少需要 2 个点。
    void drawCurve(const std::vector<JZPoint>& points);

private:
    // 粗略裁剪: 图元显示坐标包围盒与裁剪区(优先用 m_clip, 否则用屏幕)不相交则丢弃。
    bool isCulled(const JZRect& displayBounds) const;

    // 当前应下发到后端的本地变换: 仅控件自身经 setTransform/translate/rotate/scale 累积的
    // 矩阵 (以控件局部坐标为基准, 不含框架放置偏移 m_ox/m_oy)。后端据其与放置偏移、DPI 缩放
    // 合成世界变换: Scale(s) * Translate(ox,oy) * matrix。几何以本地坐标提交。
    JZMatrix2D currentTaskMatrix() const;

    // 把本地坐标路径写入绘制流 (不再在 painter 端烘焙矩阵); stroke/fill 决定描边/填充,
    // fillColor 有值取其色, 否则取当前 brush 色。变换由后端经世界变换统一施加。
    void addPathTask(const JZPainterPath& path, bool stroke, bool fill,
                     const JZOptional<JZColor>& fillColor = JZOptional<JZColor>());

    // 把当前完整状态 (pen/brush/font/clip/matrix) 以 Push 任务写入绘制流。
    void pushState();

    // 祖先裁剪: 向上找到开启 setClipChildren() 的祖先控件 (如滚动区 viewport),
    // 把显示坐标裁剪区收缩到其显示范围内; widget 为该 painter 所属控件。
    JZRect applyAncestorClips(JZWidget* widget, const JZRect& globalClip) const;

    struct State {
        JZPen pen;
        JZBrush brush;
        JZFont font;
        JZMatrix2D matrix;
    };

    JZLayer* m_layer = nullptr;
    JZWidget* m_widget = nullptr;

    // 离屏绘制 (JZPainter 作用于 JZImage) 时, 本 painter 自持一个图层, 析构时回写入 image。
    JZLayer m_ownLayer;
    JZImage* m_imageTarget = nullptr;   // 非 null = 离屏 image 模式
    JZDisplayDevice* m_deviceTarget = nullptr;   // 非 null = 离屏设备模式 (析构时回放)
    float m_scale = 1.0f;               // 离屏绘制缩放 (逻辑单位 -> image 像素; 见构造函数)

    // 绘制原点: 构造时按 widget 全局坐标自动设置, 内部用于把本地坐标平移到
    // 显示坐标 (drawXxx/setClip 通用)。不对外开放, 外部不可调整。
    int m_ox = 0;
    int m_oy = 0;
    JZRect m_clip;
    JZRect m_screen;            // 屏幕可见区域, 用于完全出屏的粗略剔除
    std::vector<State> m_stack; // save/restore 状态栈

    JZMatrix2D m_matrix;          // 当前变换矩阵: 控件本地坐标的累积变换 (每个控件绘制前为单位矩阵)

    int m_streamDepth = 0;      // 已写入绘制流、待析构 Pop 平衡的 Push 数

    JZPen m_pen;
    JZBrush m_brush;
    JZFont m_font;
};

#endif
