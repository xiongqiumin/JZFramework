#include "TokenRing.h"

#include "JZColor.h"
#include "JZFont.h"
#include "JZPen.h"
#include "JZPainter.h"

#include <algorithm>
#include <string>

using namespace std;

// 千分位：12345 -> "12,345"
static string formatCount(int v)
{
    string s = to_string(v);
    string out;
    int n = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it)
    {
        if (n > 0 && n % 3 == 0)
            out.insert(out.begin(), ',');
        out.insert(out.begin(), *it);
        n++;
    }
    return out;
}

// ==================== TokenRing ====================
TokenRing::TokenRing(JZWidget* parent)
    : JZWidget(parent)
{
    setFixedSize(26, 26);
}

int TokenRing::percent() const
{
    if (m_total <= 0)
        return 0;
    int pct = (int)((long long)m_used * 100 / m_total);
    return std::min(100, std::max(0, pct));
}

// 悬浮提示文本 (多行, '\n' 分隔), 交给 JZWidget::setTooltip 弹出
JZString TokenRing::buildTooltip() const
{
    string tip = "已用 " + formatCount(m_used) + " tokens\n";
    tip += (m_total > 0 ? ("上限 " + formatCount(m_total) + " tokens")
                        : string("上限 未设置"));
    tip += "\n";
    tip += "剩余 " + to_string(100 - percent()) + "%";
    return JZString::fromStdString(tip);
}

void TokenRing::setUsage(int used, int total)
{
    if (m_used == used && m_total == total)
        return;

    m_used = used;
    m_total = total;
    update();
    // setTooltip 在悬停中会自动刷新弹层; 未悬停则只记录, 等鼠标移入再弹
    setTooltip(buildTooltip());
}

void TokenRing::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);

    int cx = width() / 2;
    int cy = height() / 2;
    int r = std::min(width(), height()) / 2 - 2;

    // 底环
    p.setPen(JZPen(JZColor(0xd8, 0xd8, 0xd8), 3));
    p.drawArc(cx, cy, r, 0, 360);

    // 进度弧：drawArc 的角度单位是"度"，0=3 点方向、顺时针；270 即 12 点方向
    int pct = percent();
    if (pct > 0)
    {
        JZColor c = (pct >= 90) ? JZColor(0xd0, 0x40, 0x40)
                  : (pct >= 75) ? JZColor(0xe0, 0x90, 0x20)
                                : JZColor(0x2b, 0x90, 0xe0);
        p.setPen(JZPen(c, 3));
        p.drawArc(cx, cy, r, 270, 270 + 360 * pct / 100);
    }

    // 仅画环, 中心百分比数字交给悬浮提示 (setTooltip) 显示
}

JZ_OBJECT_IMPL(TokenRing, JZWidget)
