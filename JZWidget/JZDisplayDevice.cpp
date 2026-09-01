#include <algorithm>
#include <cmath>
#include "JZDisplayDevice.h"
#include "JZWidget.h"
#include "JZPainter.h"
#include "JZPainterEvent.h"
#include "JZWindow.h"
#include "JZGdiBackend.h"
#include "JZApplication.h"
#include "JZStd.h"

// ==================== 渲染调度 ====================
JZDisplayDevice::JZDisplayDevice()
{
    m_backend = new JZGdiBackend();
}

JZDisplayDevice::~JZDisplayDevice()
{
    if (m_backend) {
        m_backend->destroy();
        delete m_backend;
        m_backend = nullptr;
    }
}

void JZDisplayDevice::init(void* hwnd, int width, int height)
{
    m_backend->init(this, hwnd, width, height);
}

JZLayer* JZDisplayDevice::layer()
{
    return &m_layer;
}

float JZDisplayDevice::scale() const
{
    return m_scale;
}

void JZDisplayDevice::setScale(float scale)
{
    if (std::fabs(scale - m_scale) <= 0.001f)
        return;
    m_scale = scale;
    if (m_backend)
        m_backend->rebuildFonts();   // 字号随 DPI 缩放
}
// ==================== 渲染 ====================
// 遍历渲染树 (含剔除) -> 各控件 paintEvent 收集任务 -> backend 渲染并 Present。
void JZDisplayDevice::render(JZWidget* root, const JZRect& rc)
{
    if (!root || !m_backend)
        return;

    JZRect clip = rc.intersect(root->rect());
    if (clip.isEmpty())
        return;

    std::vector<JZWidget*> widget_list;
    std::vector<JZRect> coverage = { root->rect() };
    collectFrontToBack(root, clip, coverage, widget_list);

    std::reverse(widget_list.begin(), widget_list.end());
    // 统一创建 painter 并注入: 每个 widget 一次 paintEvent 对应一个 painter,
    // 产生的 JZDrawTask 追加到 m_layer, 最后由渲染后端一次性回放。
    for (auto w : widget_list)
    {
        JZPainter painter(w);
        painter.setClip(clip);

        JZPainterEvent event;
        event.setPainter(&painter);
        event.setRect(clip);
        w->sendEvent(&event);
    }

    m_backend->render(&m_layer);
}

// ==================== 剔除遍历 ====================
// "前到后"累计不透明覆盖, 收集需要绘制的 widget, 最后反转成"后到前"绘制顺序。
void JZDisplayDevice::collectFrontToBack(JZWidget* w, const JZRect& area, std::vector<JZRect>& coverage, std::vector<JZWidget*>& out)
{
    JZRect wa = w->mapToGlobal(w->rect()).intersect(area);

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
        collectFrontToBack(c, area, coverage, out);
    }

    // 被已覆盖矩形完全包含 -> 剔除 (不发 paintEvent / 不生成 draw_task)
    bool covered = false;
    for (const JZRect& c : coverage) {
        if (wa.left()   >= c.left() && wa.right()  <= c.right() &&
            wa.top()    >= c.top() && wa.bottom() <= c.bottom()) {
            covered = true;
            break;
        }
    }
    {
        out.push_back(w);
        coverage.push_back(wa);
    }
}