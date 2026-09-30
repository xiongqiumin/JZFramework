#include "JZVirtualDevice.h"
#include "JZVirtualWindow.h"
#include "JZImageLabel.h"
#include "JZDisplayDevice.h"

#include <algorithm>
#include <cstring>

// ==================== 虚拟桌面 ====================
// 分层: 宿主窗口 (本类, 真窗口, 可选显示)
//        └─ 桌面白屏 + 窗口合成图 (m_desktop, 一张 JZImage)
//             └─ 若干 JZVirtualWindow (无原生窗口的顶层窗口, 各有桌面坐标与 z 序)
//
// 坐标: 桌面坐标 == 屏幕逻辑坐标 (左上角原点, scale 恒为 1)。窗口有自己的桌面坐标,
// 所以 screenToClient / clientToScreen / 命中测试 / 归一化 rect 全都有意义 ——
// 与真实桌面同构, CU 的"屏幕坐标 + 窗口"语义可以直接落在这块虚拟桌面上。

namespace {

// 把 src 以 SourceOver 贴到 dst 的 (ox,oy) 处 (裁剪到边界)。
// 窗口帧是不透明的 (后端帧缓冲预填白), 所以实际总走 "a==255" 的快路径。
void jzBlitOver(uint8_t* dst, int dw, int dh,
                const uint8_t* src, int sw, int sh, int ox, int oy)
{
    if (!dst || !src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return;

    int x0 = 0, x1 = sw;
    if (ox < 0)          x0 = -ox;
    if (ox + sw > dw)    x1 = dw - ox;

    for (int y = 0; y < sh; ++y) {
        const int dy = oy + y;
        if (dy < 0 || dy >= dh)
            continue;
        const uint8_t* srow = src + (size_t)y * sw * 4;
        uint8_t* drow = dst + (size_t)dy * dw * 4;
        for (int x = x0; x < x1; ++x) {
            const uint8_t* s = srow + (size_t)x * 4;
            uint8_t* d = drow + (size_t)(ox + x) * 4;
            const unsigned a = s[3];
            if (a == 255) {
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
            } else if (a != 0) {
                const unsigned ia = 255 - a;
                d[0] = (uint8_t)((s[0] * a + d[0] * ia + 127) / 255);
                d[1] = (uint8_t)((s[1] * a + d[1] * ia + 127) / 255);
                d[2] = (uint8_t)((s[2] * a + d[2] * ia + 127) / 255);
                d[3] = 255;
            }
        }
    }
}

} // namespace

JZVirtualDevice::JZVirtualDevice(int desktopW, int desktopH, const std::string& title)
    : JZWindow(desktopW > 0 ? desktopW : 1024, desktopH > 0 ? desktopH : 768, title)
{
    m_desktopW = desktopW > 0 ? desktopW : 1024;
    m_desktopH = desktopH > 0 ? desktopH : 768;
    m_desktop = JZImage::create(m_desktopW, m_desktopH, m_desktopBg);
}

void JZVirtualDevice::setDesktopBackground(const JZColor& c)
{
    m_desktopBg = c;
    m_desktop = JZImage::create(m_desktopW, m_desktopH, m_desktopBg);
    refresh();
}

JZVirtualDevice::~JZVirtualDevice()
{
    // 桌面持有窗口 (它们没有别的父对象)
    std::vector<JZVirtualWindow*> list = m_windows;
    m_windows.clear();
    m_active = nullptr;
    for (auto* w : list)
        delete w;
}

// ==================== 多窗口管理 ====================
JZVirtualWindow* JZVirtualDevice::createWindow(int w, int h, const std::string& title)
{
    const int off = (int)m_windows.size() * 24;   // 级联偏移, 免得新窗口完全叠住旧窗口
    return createWindow(w, h, title,
                        (m_desktopW - w) / 2 + off, (m_desktopH - h) / 2 + off);
}

JZVirtualWindow* JZVirtualDevice::createWindow(int w, int h, const std::string& title,
                                               int x, int y)
{
    if (w <= 0) w = 320;
    if (h <= 0) h = 240;

    auto* win = new JZVirtualWindow(this, w, h, title);
    win->setScreenPos(x, y);
    m_windows.push_back(win);       // 新窗口置于最上
    win->show();                    // 建离屏帧缓冲 + 出首帧 (不建原生窗口)
    activate(win);                  // 置顶 + 给键盘焦点
    refresh();
    return win;
}

void JZVirtualDevice::destroyWindow(JZVirtualWindow* w)
{
    if (!w)
        return;
    auto it = std::find(m_windows.begin(), m_windows.end(), w);
    if (it == m_windows.end())
        return;

    m_windows.erase(it);
    if (m_active == w) {
        // 活动窗口没了: 退到最上层那个 (等价桌面行为)
        m_active = m_windows.empty() ? nullptr : m_windows.back();
        if (m_active)
            m_active->injectFocus(true);
    }
    delete w;
    refresh();
}

void JZVirtualDevice::raise(JZVirtualWindow* w)
{
    auto it = std::find(m_windows.begin(), m_windows.end(), w);
    if (it == m_windows.end() || it + 1 == m_windows.end())
        return;                     // 不存在, 或已经在最上
    m_windows.erase(it);
    m_windows.push_back(w);
    refresh();
}

void JZVirtualDevice::activate(JZVirtualWindow* w)
{
    if (!w)
        return;
    raise(w);

    if (m_active == w) {
        if (!w->isActive())
            w->injectFocus(true);
        return;
    }
    if (m_active)
        m_active->injectFocus(false);   // 旧活动窗口失焦 (派发 focusOut, 取消未上屏的编辑)
    m_active = w;
    w->injectFocus(true);
    refresh();
}

void JZVirtualDevice::moveWindow(JZVirtualWindow* w, int x, int y)
{
    if (w)
        w->setScreenPos(x, y);          // 窗口自己会回调 onWindowFrameChanged -> 重新合成
}

JZVirtualWindow* JZVirtualDevice::byTitle(const std::string& title) const
{
    if (title.empty())
        return nullptr;
    for (auto it = m_windows.rbegin(); it != m_windows.rend(); ++it) {
        if ((*it)->title().find(title) != std::string::npos)
            return *it;
    }
    return nullptr;
}

JZVirtualWindow* JZVirtualDevice::windowAt(int screenX, int screenY) const
{
    // z 序自上而下: 末尾最上
    for (auto it = m_windows.rbegin(); it != m_windows.rend(); ++it) {
        JZVirtualWindow* w = *it;
        if (!w->isVisible())
            continue;
        const JZRect g = w->screenGeometry();
        if (screenX >= g.left() && screenX <= g.right() &&
            screenY >= g.top()  && screenY <= g.bottom())
            return w;
    }
    return nullptr;
}

// ==================== 坐标换算 ====================
JZPoint JZVirtualDevice::screenToClient(JZVirtualWindow* w, int screenX, int screenY) const
{
    if (!w)
        return JZPoint(screenX, screenY);
    return JZPoint(screenX - w->screenX(), screenY - w->screenY());
}

JZPoint JZVirtualDevice::clientToScreen(JZVirtualWindow* w, int clientX, int clientY) const
{
    if (!w)
        return JZPoint(clientX, clientY);
    return JZPoint(clientX + w->screenX(), clientY + w->screenY());
}

// ==================== 合成 / 取帧 ====================
void JZVirtualDevice::compose()
{
    if (m_composing)
        return;
    m_composing = true;

    // 每帧从"桌面底色"起画 (窗口可能被移走/关掉了, 旧像素必须被盖掉)。
    // 直接重建整块: 不依赖 JZColor 的取值接口, 也避免"按色号分别填充"的分支。
    m_desktop = JZImage::create(m_desktopW, m_desktopH, m_desktopBg);

    uint8_t* dst = m_desktop.data() ? m_desktop.data()->pixels() : nullptr;
    if (dst) {
        for (auto* w : m_windows) {          // z 序: 由下到上
            if (!w->isVisible())
                continue;
            // 用窗口**现有的**帧缓冲 (窗口内容变化时由它自己重绘并回调刷新),
            // 因此合成不会触发"渲染 -> 通知 -> 合成"的回环。
            JZImage f = w->snapshot(false);
            if (f.isNull() || !f.data())
                continue;
            jzBlitOver(dst, m_desktopW, m_desktopH,
                       f.data()->pixels(), f.width(), f.height(),
                       w->screenX(), w->screenY());
        }
    }

    m_composing = false;
}

void JZVirtualDevice::refresh()
{
    compose();
    if (m_view) {                        // 宿主窗口显示中: 把合成图贴出去 (没有则纯无头)
        m_view->setImage(m_desktop);
        m_view->update();
    }
}

JZImage JZVirtualDevice::screenshot()
{
    refresh();
    return m_desktop;                    // 与 m_desktop 共享像素缓冲, 拷贝很轻
}

JZImage JZVirtualDevice::screenshotWindow(JZVirtualWindow* w)
{
    return w ? w->snapshot(true) : JZImage();
}

void JZVirtualDevice::onWindowFrameChanged(JZVirtualWindow*)
{
    if (m_composing)
        return;                          // 合成过程中的通知忽略 (防重入)
    refresh();
}

// ==================== 输入注入 ====================
bool JZVirtualDevice::injectMouseAtScreen(JZMouseEvent::MouseType type,
                                          JZMouseEvent::Button button,
                                          int screenX, int screenY,
                                          int wheelDelta, uint32_t modifiers)
{
    JZVirtualWindow* w = windowAt(screenX, screenY);
    if (!w)
        return false;                    // 点在桌面白屏上: 不归任何窗口 (等价真实桌面)

    if (type == JZMouseEvent::Press)
        activate(w);                     // 点击激活窗口 (与真实桌面一致)

    const JZPoint p = screenToClient(w, screenX, screenY);
    return w->injectMouse(type, button, p.x(), p.y(), wheelDelta, modifiers);
}

bool JZVirtualDevice::injectClickAt(int screenX, int screenY, JZMouseEvent::Button button)
{
    const bool down = injectMouseAtScreen(JZMouseEvent::Press, button, screenX, screenY);
    const bool up   = injectMouseAtScreen(JZMouseEvent::Release, button, screenX, screenY);
    return down || up;
}

bool JZVirtualDevice::injectKey(uint32_t key, const std::string& text,
                                uint32_t modifiers, bool press)
{
    return m_active ? m_active->injectKey(key, text, modifiers, press) : false;
}

bool JZVirtualDevice::injectText(const std::string& utf8)
{
    if (!m_active)
        return false;

    bool any = false;
    size_t i = 0;
    while (i < utf8.size()) {
        const unsigned char c0 = (unsigned char)utf8[i];
        size_t len = 1;
        uint32_t cp = c0;
        if (c0 >= 0xF0)      { len = 4; cp = c0 & 0x07u; }
        else if (c0 >= 0xE0) { len = 3; cp = c0 & 0x0Fu; }
        else if (c0 >= 0xC0) { len = 2; cp = c0 & 0x1Fu; }
        if (i + len > utf8.size())
            len = 1;
        if (len > 1) {
            for (size_t k = 1; k < len; ++k)
                cp = (cp << 6) | ((unsigned char)utf8[i + k] & 0x3Fu);
        }
        const std::string ch = utf8.substr(i, len);
        injectKey(cp, ch, JZKeyEvent::NoModifier, true);
        injectKey(cp, ch, JZKeyEvent::NoModifier, false);
        any = true;
        i += len;
    }
    return any;
}

// ==================== 宿主窗口 (可选显示) ====================
JZImageLabel* JZVirtualDevice::ensureView()
{
    if (!m_view) {
        m_view = new JZImageLabel(this);
        m_view->setGeometry(0, 0, width(), height());
        m_view->setImage(m_desktop);
        m_view->show();
    }
    return m_view;
}

void JZVirtualDevice::showEvent(JZShowEvent* event)
{
    // 宿主窗口是真的顶层窗口: 建原生窗口并显示 (供人查看)
    JZWindow::showEvent(event);
    ensureView();
    refresh();
}

void JZVirtualDevice::resize(int w, int h)
{
    JZWindow::resize(w, h);
    if (m_view)
        m_view->setGeometry(0, 0, w, h);
}
