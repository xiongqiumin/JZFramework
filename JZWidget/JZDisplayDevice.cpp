#include <algorithm>
#include <cmath>
#include "JZDisplayDevice.h"
#include "JZWidget.h"
#include "JZPainter.h"
#include "JZPainterEvent.h"
#include "JZWindow.h"
#include "JZGdiBackend.h"
#include "JZPixmap.h"
#include "JZApplication.h"
#include "JZStd.h"

// ==================== 渲染调度 ====================
JZDisplayDevice::JZDisplayDevice()
{
    m_backend = new JZGdiBackend();   // 默认后端; 可经 setRenderBackend 替换
}

JZDisplayDevice::~JZDisplayDevice()
{
    if (m_backend) {
        m_backend->destroy();
        delete m_backend;
        m_backend = nullptr;
    }
}

void JZDisplayDevice::setRenderBackend(JZRenderBackend* backend)
{
    if (backend == m_backend)
        return;
    if (m_backend) {
        m_backend->destroy();
        delete m_backend;
    }
    m_backend = backend;
    if (!m_backend)
        m_backend = new JZGdiBackend();   // nullptr = 恢复默认后端
}

void JZDisplayDevice::init(void* hwnd, int width, int height)
{
    if (m_backend)
        m_backend->init(this, hwnd, (unsigned)width, (unsigned)height);
}

// 离屏: 不绑定窗口, 后端只建自己的帧缓冲 (target = nullptr), 取帧走 snapshot
void JZDisplayDevice::initOffscreen(int width, int height)
{
    if (m_backend)
        m_backend->init(this, nullptr, (unsigned)width, (unsigned)height);
}

bool JZDisplayDevice::snapshot(JZImage& out)
{
    return m_backend ? m_backend->snapshot(out) : false;
}

JZLayer* JZDisplayDevice::layer()
{
    return &m_layer;
}

float JZDisplayDevice::scale() const
{
    return m_scale;
}

// 回放一份现成的绘制流 (画布型离屏设备; 见头文件)。与窗口那条路的差别只有"没有控件树":
// 任务里的坐标是逻辑的, 物理换算照旧由后端用本设备的 DPI 缩放施加。
void JZDisplayDevice::renderLayer(JZLayer* layer)
{
    if (!m_backend || !layer)
        return;
    m_backend->render(layer);
}

// 帧缓冲 -> 显示位图 (零拷贝引用): 后端在自己的原生缓冲上画完, 显示侧直接贴它,
// 中间不做任何 RGBA<->PARGB 转换。缓冲随 resize/析构重建, 所以引用方必须是
// "每次贴之前重新取"或用同一生命周期持有 (见 JZVirtualDevice::desktopPixmap)。
bool JZDisplayDevice::targetPixmap(JZPixmap& out) const
{
    out = JZPixmap();
    if (!m_backend)
        return false;
    int w = 0, h = 0, stride = 0;
    const uint8_t* px = m_backend->targetPixels(w, h, stride);
    if (!px || w <= 0 || h <= 0 || stride != w * 4)
        return false;                 // 不支持的布局: 调用方退回 snapshot + JZPixmap::fromImage
    std::shared_ptr<JZPixmapData> d = std::make_shared<JZPixmapData>();
    d->setPixelsRef(w, h, px);
    out = JZPixmap::fromData(d);
    return true;
}

void JZDisplayDevice::setScale(float scale)
{
    if (std::fabs(scale - m_scale) <= 0.001f)
        return;
    m_scale = scale;
    if (m_backend)
        m_backend->rebuildFonts();   // 字号随 DPI 缩放
}

void JZDisplayDevice::resize(int width, int height)
{
    if (m_backend)
        m_backend->resizeSwapChain((unsigned)width, (unsigned)height);
}
// ==================== 渲染 ====================
// 遍历渲染树 (含剔除) -> 各控件 paintEvent 收集任务 -> backend 渲染并 Present。
void JZDisplayDevice::render(JZWidget* root, const JZRect& rc)
{
    if (!root || !m_backend)
        return;

    // 整窗重绘: 忽略传入的脏区 rc, 每帧重画整窗。
    // 现代 Windows/DWM 下呈现本就整屏合成, 应用层脏区只省 GDI+ 绘制,
    // 且易因漏算旧位产生残影; 直接全量重绘, 上层 invalidate/合并逻辑保持不变。
    JZRect clip = root->rect();
    if (clip.isEmpty())
        return;

    // 记录本轮脏区: JZPainter 构造时取 dirty ∩ 控件 rect 作为裁剪
    // (控件在 paintEvent 里自建 JZPainter, clip 权威设定点在构造函数)
    m_dirty = clip;

    std::vector<JZWidget*> widget_list;
    // 前到后累计"身前不透明覆盖"。初始必须为空: 若把整窗(根)预先放进 coverage,
    // 每个子控件的 wa ⊆ 整窗矩形, 一进 collect 就被判定"已被覆盖"而剔除,
    // 连根自己也被覆盖 -> widget_list 为空 -> 整窗白屏。
    std::vector<JZRect> coverage;

    // ancClip 初值 = 全窗口区域; 视图裁剪只会把可绘制区收得更小
    collectFrontToBack(root, clip, coverage, widget_list, clip);

    std::reverse(widget_list.begin(), widget_list.end());
    // 逐个派发 paintEvent: 每个控件在自己的 paintEvent 里自建 JZPainter
    // (构造时按"脏区 ∩ 控件全局 rect"设好权威裁剪, Qt 语义; 见 JZPainter.cpp),
    // 产生的 JZDrawTask 追加到 m_layer, 最后由渲染后端一次性回放。
    // 事件只带重绘区域, 不携带画笔。
    for (auto w : widget_list)
    {
        JZPainterEvent event;
        event.setRect(clip);
        jzApp->sendEvent(w, &event);   // 同步派发, paint 同样过控件过滤器链 (同 Qt);
    }

    m_backend->render(&m_layer);
}

// ==================== 剔除遍历 ====================
// "前到后"累计不透明覆盖, 收集需要绘制的 widget, 最后反转成"后到前"绘制顺序。
// ancClip: 祖先 clipChildren 控件叠加收窄出的可绘制区 (窗口坐标)。若子控件落在
// 这些裁剪祖先之外, 它画不出来; 因此其"覆盖贡献"也只算可视区, 不能拿完整 rect
// 去剔除身后的兄弟控件 (典型场景: 视口里的超大内容子控件不能把滚动条整没)。
void JZDisplayDevice::collectFrontToBack(JZWidget* w, const JZRect& area, std::vector<JZRect>& coverage, std::vector<JZWidget*>& out, const JZRect& ancClip)
{
    // 本节点可绘制区 = 自身 rect ∩ 脏区 ∩ 祖先裁剪
    JZRect wa = w->mapTo(w->window(), w->rect()).intersect(area).intersect(ancClip);

    // 先处理子节点(前到后), 使上层子节点先贡献覆盖, 再判断本节点(本节点在子节点之后)
    // 注意: 挂在本树下的其它窗口(如对话框)有各自的渲染设备, 不属于本可视树, 跳过;
    //       隐藏控件不参与绘制, 也不贡献覆盖。
    const auto& ch = w->children();
    for (int i = (int)ch.size() - 1; i >= 0; --i) {
        auto* c = jzobject_cast<JZWidget*>(ch[i]);
        if (!c || jzobject_cast<JZWindow*>(c))
            continue;
        if (!c->isVisible())
            continue;
        // clipChildren 语义 (与 JZPainter::applyAncestorClips 一致): 子节点可绘制
        // 区再被本节点收窄; 否则继承上面的祖先裁剪即可。
        JZRect childClip = w->clipChildren()
            ? w->mapTo(w->window(), w->rect()).intersect(ancClip)
            : ancClip;
        collectFrontToBack(c, area, coverage, out, childClip);
    }

    // 无任何可绘制像素 -> 不必画
    if (wa.isEmpty())
        return;

    // 被已覆盖矩形完全包含 -> 剔除 (不发 paintEvent / 不生成 draw_task)
    bool covered = false;
    for (const JZRect& c : coverage) {
        if (wa.left()   >= c.left() && wa.right()  <= c.right() &&
            wa.top()    >= c.top() && wa.bottom() <= c.bottom()) {
            covered = true;
            break;
        }
    }
    if (!covered) {
        out.push_back(w);
        // 只有"不透明"控件才向身后的兄弟贡献覆盖 (全屏蒙版/涟漪等透明覆盖层必须
        // 放行身后内容重绘 —— 否则冻结在帧缓冲里的旧像素不会被擦除, 移动中的
        // AI 光标/涟漪会在身后内容上留下残影)。
        if (w->opaque() > 0)
            coverage.push_back(wa);
    }
}