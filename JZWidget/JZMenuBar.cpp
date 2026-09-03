#include "JZMenuBar.h"
#include "JZMenu.h"
#include "JZPainter.h"
#include "JZFont.h"
#include "JZFontMetrics.h"
#include "JZPen.h"
#include "JZBrush.h"
#include "JZColor.h"

static const int kBarFont = 13;
static const int kBarHeight = 26;
static const int kTitlePadX = 8;   // 标题左右内边距
static const int kTitleGap = 4;    // 标题之间间隔

JZ_OBJECT_IMPL(JZMenuBar, JZWidget)

JZMenuBar::JZMenuBar(JZWidget* parent)
    : JZWidget(parent)
{
}

JZMenuBar::~JZMenuBar()
{
    for (auto* m : m_menus)
        delete m;
    m_menus.clear();
}

JZSize JZMenuBar::sizeHint() const
{
    int w = m_width > 0 ? m_width : 200;
    return JZSize(w, kBarHeight);
}

JZMenu* JZMenuBar::addMenu(const std::string& title)
{
    JZMenu* m = new JZMenu(title, nullptr);   // 由本菜单栏在析构时统一释放
    m_menus.push_back(m);
    return m;
}

int JZMenuBar::menuCount() const
{
    return (int)m_menus.size();
}

JZMenu* JZMenuBar::menuAt(int index) const
{
    if (index >= 0 && index < (int)m_menus.size())
        return m_menus[index];
    return nullptr;
}

std::vector<JZRect> JZMenuBar::titleRects() const
{
    JZFont font(kBarFont);
    std::vector<JZRect> rects;
    int x = kTitleGap;
    int h = height() > 0 ? height() : kBarHeight;
    for (auto* m : m_menus) {
        int tw = JZFontMetrics::textWidth(m->title(), font);
        int w = tw + kTitlePadX * 2;
        rects.push_back(JZRect(x, 0, w, h));
        x += w + kTitleGap;
    }
    return rects;
}

void JZMenuBar::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);
    int w = width(), h = height();

    p.fillRect(0, 0, w, h, JZColor(0xf2, 0xf2, 0xf2));
    p.setPen(JZPen(JZColor(0xc8, 0xc8, 0xc8), 1));
    p.drawLine(0, h - 1, w, h - 1);

    auto rects = titleRects();
    JZFont font(kBarFont);
    for (int i = 0; i < (int)rects.size(); ++i) {
        bool hot = (i == m_hoverIndex) || (i == m_openIndex);
        JZRect r = rects[i];
        if (hot)
            p.fillRect(r.left(), r.top(), r.width(), r.height(), JZColor(0xe2, 0xe2, 0xe2));

        p.setPen(JZPen(JZColor(0x20, 0x20, 0x20), 1));
        p.setFont(font);
        int tx = JZFontMetrics::centerX(m_menus[i]->title(), font, r.width()) + r.left();
        int ty = JZFontMetrics::centerY(font, h);
        p.drawText(tx, ty, m_menus[i]->title().c_str());
    }
}

void JZMenuBar::mousePressEvent(JZMouseEvent* event)
{
    auto rects = titleRects();
    int mx = event->localPos().x();
    int my = event->localPos().y();
    for (int i = 0; i < (int)rects.size(); ++i) {
        if (rects[i].contains(mx, my)) {
            openMenu(i);
            return;
        }
    }
}

void JZMenuBar::mouseMoveEvent(JZMouseEvent* event)
{
    auto rects = titleRects();
    int mx = event->localPos().x();
    int my = event->localPos().y();
    int hit = -1;
    for (int i = 0; i < (int)rects.size(); ++i) {
        if (rects[i].contains(mx, my)) {
            hit = i;
            break;
        }
    }
    if (hit != m_hoverIndex) {
        m_hoverIndex = hit;
        update();
    }
}

void JZMenuBar::openMenu(int index)
{
    if (index < 0 || index >= (int)m_menus.size())
        return;
    JZMenu* menu = m_menus[index];
    JZWindow* owner = window();
    if (!owner)
        return;

    auto rects = titleRects();
    JZRect tr = rects[index];
    JZPoint tl = mapToGlobal(tr.topLeft());
    JZRect anchor(tl.x(), tl.y(), tl.x() + tr.width(), tl.y() + tr.height());

    m_openIndex = index;
    update();
    menu->popup(owner, anchor, false);   // 阻塞直到菜单关闭
    m_openIndex = -1;
    update();
}

std::vector<std::string> JZMenuBar::aiItems() const
{
    std::vector<std::string> v;
    for (auto* m : m_menus)
        v.push_back(m->title());
    return v;
}
