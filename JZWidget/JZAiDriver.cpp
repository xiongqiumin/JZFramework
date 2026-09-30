#include "JZAiDriver.h"
#include "JZWindow.h"
#include "JZPushButton.h"
#include "JZPainter.h"
#include "JZFontMetrics.h"
#include "JZMouseEvent.h"
#include "JZThread.h"
#include "JZApplication.h"

#include <algorithm>
#include <cmath>

JZ_OBJECT_IMPL(JZAiOverlay, JZWidget)
JZ_OBJECT_IMPL(JZAiDriver, JZObject)

// ==================== 覆盖层 ====================

JZAiOverlay::JZAiOverlay(JZWidget* parent)
    : JZWidget(parent)
    , sigCancelRequested(this)
    , m_cursorPos(30, 30)   // AI 光标初始位置: 左上角
{
    // 全窗蒙版 (见 paintEvent 的半透明遮罩) + 命中透传: 自身不参与 hitTest,
    // AI 合成点击可穿到目标控件; 子控件 (取消按钮) 正常参与命中。
    // 真实用户输入由 JZWindow::setInputGate 在蒙版可见期间整体屏蔽。
    setPassThrough(true);
    // 不向身后内容贡献"不透明覆盖": 否则重绘剔除把蒙版下的内容冻结成上帧,
    // 既让移动的 AI 光标留残影, 又让半透明遮罩逐帧叠加变黑。
    setOpaque(0);

    m_cancelBtn = new JZPushButton("取消", this);
}

void JZAiOverlay::setCursorPos(const JZPoint& pos)
{
    m_cursorPos = pos;
    update();
}

void JZAiOverlay::setStatusText(const std::string& text)
{
    m_statusText = text;
    layoutBanner();
    update();
}

void JZAiOverlay::startClickEffect(const JZPoint& pos)
{
    m_effectOn = true;
    m_effectPos = pos;
    m_effectTick = 0;
    update();
}

void JZAiOverlay::tickEffect()
{
    if (!m_effectOn)
        return;
    if (++m_effectTick > 12)
        m_effectOn = false;
    update();
}

// 尺寸对齐父窗口; 尺寸变化时对"新旧区域并集"失效 —— 覆盖层长高后
// 原区域外若残留旧帧像素 (如旧状态条文字), 不会被 update(rect) 覆盖到
void JZAiOverlay::syncToParent()
{
    JZWidget* p = parentWidget();
    if (!p)
        return;
    JZRect oldRc = rect();
    setGeometry(0, 0, p->width(), p->height());
    JZRect nowRc = rect();
    if (nowRc.x() != oldRc.x() || nowRc.y() != oldRc.y() ||
        nowRc.width() != oldRc.width() || nowRc.height() != oldRc.height())
        invalidate(oldRc.united(nowRc));
    layoutBanner();
}

// 状态条布局 (左下角): 与绘制分离, 在文本/尺寸变化时确定性地就位
void JZAiOverlay::layoutBanner()
{
    if (m_statusText.empty()) {
        m_bannerRect = JZRect();
        m_cancelBtn->setVisible(false);
        return;
    }

    JZFont font(14);
    int textW = JZFontMetrics::textWidth(m_statusText, font);
    int bh = 34;
    int bx = 12;
    int by = std::max(0, height() - bh - 12);
    int bw = textW + 84;   // 文本 + 取消按钮区域

    m_bannerRect = JZRect(bx, by, bw, bh);
    m_cancelBtn->setGeometry(bx + 4, by + 4, 54, bh - 8);
    m_cancelBtn->setVisible(true);
}

// 绘制只读 m_bannerRect, 不做任何几何变更
void JZAiOverlay::paintEvent(JZPainterEvent*)
{
    JZPainter p(this);

    // ---- 全屏蒙版: 半透明遮罩铺满全窗 (蒙版下内容每帧重绘, 见 setOpaque(0)),
    // 既传达"操作中"的状态, 也压暗下方控件让 AI 光标/涟漪更醒目 ----
    p.fillRect(rect(), JZColor(0x10, 0x13, 0x1a, 96));

    // ---- 状态条 (左下角, 半透明深色圆角条) ----
    if (!m_bannerRect.isEmpty()) {
        JZFont font(14);
        p.setFont(font);
        p.setBrush(JZBrush(JZColor(0x22, 0x24, 0x2a, 225)));
        p.setPen(JZPen(JZColor(0x22, 0x24, 0x2a, 225), 1));
        p.drawRoundRect(m_bannerRect.x(), m_bannerRect.y(),
                        m_bannerRect.width(), m_bannerRect.height(), 8);
        p.setPen(JZPen(JZColor(0xff, 0xff, 0xff), 1));
        p.drawText(m_bannerRect.x() + 4 + 54 + 10,
                   m_bannerRect.y() + JZFontMetrics::centerY(font, m_bannerRect.height()),
                   m_statusText.c_str());
    }

    // ---- 点击涟漪 (扩散圆环, 渐隐) ----
    if (m_effectOn) {
        int r = 8 + m_effectTick * 3;
        int alpha = std::max(40, 220 - m_effectTick * 22);
        p.setPen(JZPen(JZColor(0x33, 0x66, 0xcc, (uint8_t)alpha), 2));
        p.drawArc(m_effectPos.x(), m_effectPos.y(), r, 0, 3600);
        p.setPen(JZPen(JZColor(0x33, 0x66, 0xcc, (uint8_t)(alpha / 2)), 2));
        p.drawArc(m_effectPos.x(), m_effectPos.y(), r / 2 + 2, 0, 3600);
    }

    // ---- AI 光标 (经典箭头, 白底黑边) ----
    drawArrow(p, m_cursorPos);
}

// 经典箭头: 多边形扫描线填充, 两遍 (黑描边 + 白色本体)
void JZAiOverlay::drawArrow(JZPainter& p, const JZPoint& pos) const
{
    static const JZPoint pts[] = {
        {0, 0}, {0, 17}, {4, 13}, {7, 20}, {10, 19}, {7, 12}, {12, 12}
    };
    const int n = (int)(sizeof(pts) / sizeof(pts[0]));

    for (int pass = 0; pass < 2; ++pass) {
        JZColor col = (pass == 0) ? JZColor(0x20, 0x20, 0x20) : JZColor(0xff, 0xff, 0xff);
        int grow = (pass == 0) ? 1 : 0;   // 第一遍外扩 1px 充当描边
        for (int y = 0; y <= 20; ++y) {
            int xs[8];
            int cnt = 0;
            for (int i = 0; i < n; ++i) {
                const JZPoint& a = pts[i];
                const JZPoint& b = pts[(i + 1) % n];
                if ((a.y() <= y && b.y() > y) || (b.y() <= y && a.y() > y)) {
                    xs[cnt++] = a.x() + (b.x() - a.x()) * (y - a.y()) / (b.y() - a.y());
                }
            }
            std::sort(xs, xs + cnt);
            for (int k = 0; k + 1 < cnt; k += 2)
                p.fillRect(pos.x() + xs[k] - grow, pos.y() + y - grow,
                           xs[k + 1] - xs[k] + 1 + grow * 2, 1, col);
        }
    }
}

void JZAiOverlay::keyPressEvent(JZKeyEvent* event)
{
    // 蒙版期间 Esc = 取消, 其余键吞掉 (不转发给基类)
    if (event->key() == Key_Escape)
        sigCancelRequested.emit();
}

// ==================== 驱动 ====================

JZAiDriver::JZAiDriver(JZWindow* root, JZObject* parent)
    : JZObject(parent)
    , sigDone(this)
    , m_root(root)
{
    // 覆盖层**懒建**: 只有可视化打开时才有 (默认关闭, 见 setVisualEnabled)
    m_timer = new JZTimer(this);
    connect(&m_timer->sigTimeout, this, &JZAiDriver::onTick);
}

JZAiOverlay* JZAiDriver::ensureOverlay()
{
    if (!m_visual || !m_root)
        return m_overlay;
    if (!m_overlay)
    {
        m_overlay = new JZAiOverlay(m_root);
        m_overlay->hide();
        connect(&m_overlay->sigCancelRequested, this, &JZAiDriver::cancel);
    }
    return m_overlay;
}

int JZAiDriver::moveToWidget(JZWidget* target, bool performClick,
                             const std::string& statusText, std::function<void()> onArrive)
{
    if (m_phase != PhaseIdle || !target || !m_root || !m_root->nativeHandle())
        return -1;   // 忙 / 目标无效

    ++m_seq;
    m_cancelRequested = false;
    m_performClick = performClick;
    m_onArrive = std::move(onArrive);
    m_from = m_cursorPos;
    m_to = target->mapTo(target->window(), JZPoint(target->width() / 2, target->height() / 2));

    if (!m_visual)
    {
        // ---- 无可视化: 不建覆盖层 / 不屏蔽输入 / 不等动画 ----
        // 仍然经 runOnObject 投递到事件派发上下文再执行动作 —— 与"到达后"完全同一条路径,
        // 这样合成点击/onArrive 的行为在两种模式下一致(只是快), 也不会嵌在调用栈里执行。
        m_cursorPos = m_to;
        m_phase = PhaseActing;
        m_actTicks = -1;                       // 语义同动画路径: 动作还没执行
        runOnObject(this, [this]() { doArrive(); });
        return m_seq;
    }

    m_overlay->syncToParent();
    m_overlay->setStatusText(statusText);
    m_overlay->setCursorPos(m_cursorPos);

    // 全屏蒙版上闸: 窗口丢弃真实鼠标/键盘/IME (仅"取消"按钮与 Esc 有效),
    // AI 的合成点击标记 programmatic, 不受屏蔽, 可穿透蒙版落到目标。
    m_root->cancelImeComposition();   // 丢弃在途的组字
    m_root->setInputGate(m_overlay);
    m_overlay->show();

    // 移动时长随距离走 (慢速, 有"过程感"): 300~1200ms
    int dist = std::abs(m_to.x() - m_from.x()) + std::abs(m_to.y() - m_from.y());
    int duration = std::min(1200, std::max(300, dist * 3 / 2));
    m_totalTicks = std::max(1, duration / kTickMs);
    m_tick = 0;
    m_phase = PhaseMoving;
    m_timer->start(kTickMs);
    return m_seq;
}

void JZAiDriver::cancel()
{
    if (m_phase == PhaseIdle)
        return;
    m_cancelRequested = true;       // 动画模式下由下个 tick 消费
    if (!m_visual)
        finish(false, "操作已被取消");   // 无可视化: 没有 tick 可等, 立即收尾(否则会一直"忙")
}

// 旧根窗口可能已被 JS 关窗销毁: 只比指针判断是否还在顶层窗口表里, 绝不访问对象
static bool isTopLevelAlive(JZWindow* w)
{
    if (!w || !jzApp)
        return false;
    for (JZWindow* x : jzApp->topLevelWindows())
    {
        if (x == w)
            return true;
    }
    return false;
}

void JZAiDriver::setRoot(JZWindow* root)
{
    if (root == m_root)
        return;

    if (m_phase != PhaseIdle)
    {
        // 作废当前序列: 停表 + 解除旧窗口的输入屏蔽 + 回报取消
        m_timer->stop();
        if (isTopLevelAlive(m_root))
            m_root->setInputGate(nullptr);
        m_phase = PhaseIdle;
        m_onArrive = nullptr;
        sigDone.emit(m_seq, false, "目标窗口已切换, 操作取消");
    }

    if (m_overlay)
        m_overlay->hide();

    m_root = root;
    if (!m_root)
        return;

    if (m_overlay)
    {
        m_overlay->setParentWidget(m_root);   // 覆盖层随目标窗口走 (由新的根窗口拥有)
        m_overlay->syncToParent();
    }
    else
    {
        ensureOverlay();   // 仅当可视化为开才建; 关着就一直不建覆盖层
    }
}

// 开关可视化 (默认关, 见头文件注释: 截图干净 + 立即执行 + 不屏蔽真实输入)
void JZAiDriver::setVisualEnabled(bool on)
{
    if (m_visual == on)
        return;

    m_visual = on;
    if (on)
    {
        ensureOverlay();
        if (m_overlay)
            m_overlay->syncToParent();
        return;
    }

    // 关掉: 立刻收起覆盖层 + 解除输入屏蔽 (别把真实输入锁在蒙版后面)
    if (m_overlay)
        m_overlay->hide();
    if (m_root && isTopLevelAlive(m_root))
        m_root->setInputGate(nullptr);
    m_cursorPos = m_to;   // 位置跟上, 免得下次开启时从旧位置"飞"过来
}

void JZAiDriver::onTick()
{
    if (m_phase == PhaseIdle) {
        m_timer->stop();
        return;
    }
    if (m_cancelRequested) {
        finish(false, "操作已被用户取消");
        return;
    }

    if (m_phase == PhaseMoving) {
        ++m_tick;
        float t = (m_totalTicks <= 0) ? 1.0f : (float)m_tick / m_totalTicks;
        t = std::min(1.0f, t);
        float e = t * t * (3.0f - 2.0f * t);   // smoothstep 缓动
        m_cursorPos = JZPoint(
            m_from.x() + (int)std::lround((m_to.x() - m_from.x()) * e),
            m_from.y() + (int)std::lround((m_to.y() - m_from.y()) * e));
        if (m_overlay)
            m_overlay->setCursorPos(m_cursorPos);

        if (m_tick >= m_totalTicks) {
            // 到达目标: 动作经 doArrive 投递到事件派发上下文执行 (脱离 timer 栈,
            // 防止 onArrive 里的模态循环与 processTimers 重入互踩), 本 tick 结束。
            // m_actTicks < 0 表示停留计时等 doArrive 执行完才开始
            m_phase = PhaseActing;
            m_actTicks = -1;
            runOnObject(this, [this]() { doArrive(); });
        }
        return;
    }

    // PhaseActing: 等待 doArrive 执行 (m_actTicks < 0), 之后涟漪/停留展示, 结束回报
    if (m_actTicks < 0)
        return;
    if (m_overlay)
        m_overlay->tickEffect();
    if (--m_actTicks <= 0)
        finish(true, "ok");
}

// 到达后的动作 (事件派发上下文): click 类先出涟漪再合成真实点击; 其它直调
void JZAiDriver::doArrive()
{
    if (m_phase == PhaseIdle || m_cancelRequested)   // 投递期间被取消/结束: 丢弃动作
        return;
    if (m_performClick) {
        if (m_overlay)
            m_overlay->startClickEffect(m_to);   // 涟漪只在可视化模式下有
        // 合成点击投递到事件队列 (JZWindow::event 派发): 按钮槽 —— 及其可能
        // 打开的模态框 —— 在独立派发上下文执行, 不阻塞本函数, AI 序列立即
        // 收尾回报 (回报语义 = 点击已落地, 而非点击引发的模态流结束)
        // m_to 为 m_root 客户区坐标, 合成事件需屏幕逻辑坐标 (Qt globalPos 语义)
        JZPoint gTo = m_root->mapToGlobal(m_to);
        JZMouseEvent* press = new JZMouseEvent(JZMouseEvent::Press, JZMouseEvent::LeftButton,
                                               gTo, JZPoint(), 0);
        press->setProgrammatic(true);   // AI 合成输入: 穿透全屏蒙版 (不丢给屏蔽)
        jzApp->postEvent(m_root, press);
        JZMouseEvent* release = new JZMouseEvent(JZMouseEvent::Release, JZMouseEvent::LeftButton,
                                                 gTo, JZPoint(), 0);
        release->setProgrammatic(true);
        jzApp->postEvent(m_root, release);
    }
    if (m_onArrive) {
        auto fn = std::move(m_onArrive);
        m_onArrive = nullptr;
        fn();
    }
    if (m_visual)
        m_actTicks = kActTicks;   // 动作完成, 开始停留展示计时(等 onTick 收尾)
    else
        finish(true, "ok");       // 无可视化: 不等展示, 立即回报(评测里每次 invoke 省 ~0.4s)
}

void JZAiDriver::finish(bool ok, const std::string& msg)
{
    m_timer->stop();
    m_phase = PhaseIdle;
    m_onArrive = nullptr;
    // 根窗口可能已被 JS 关窗销毁: 只比指针, 不访问对象
    if (m_root && isTopLevelAlive(m_root))
        m_root->setInputGate(nullptr);   // 解除输入屏蔽, 恢复真实输入
    if (m_overlay)
        m_overlay->hide();
    sigDone.emit(m_seq, ok, msg);
}
