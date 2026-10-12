#include "JZGameView.h"
#include "JZDebug.h"

JZGameView::JZGameView(JZWidget* parent)
    : JZWidget(parent)
{
    // 两个 timer 都是本控件的子对象 (JZObject 父子树), 这里仍显式 stop+delete (同 shooter 的姿势)
    m_logicTimer  = new JZFixedTimer(this);
    m_renderTimer = new JZTimer(this);
    connect(&m_logicTimer->sigTimeout, this, &JZGameView::onLogicTick);
    connect(&m_logicTimer->sigError,   this, &JZGameView::onLogicErrorSlot);
    connect(&m_renderTimer->sigTimeout, this, &JZGameView::onRenderTick);
}

JZGameView::~JZGameView()
{
    if (m_logicTimer)  { m_logicTimer->stop();  delete m_logicTimer;  m_logicTimer = nullptr; }
    if (m_renderTimer) { m_renderTimer->stop(); delete m_renderTimer; m_renderTimer = nullptr; }
}

// ---- 参数 ----
void JZGameView::setLogicInterval(int ms)
{
    m_logicMs = ms > 0 ? ms : 16;
    if (m_logicTimer && m_logicTimer->isActive())
        m_logicTimer->start(m_logicMs, m_maxCatchUp);   // 运行中改: 按新步长重起
}

int JZGameView::logicInterval() const
{
    return m_logicMs;
}

void JZGameView::setMaxCatchUp(int frames)
{
    m_maxCatchUp = frames > 0 ? frames : 1;
}

int JZGameView::maxCatchUp() const
{
    return m_maxCatchUp;
}

void JZGameView::setRenderInterval(int ms)
{
    m_renderMs = ms >= 0 ? ms : 0;
    if (m_renderTimer && m_renderTimer->isActive())
        m_renderTimer->start(m_renderMs);
}

int JZGameView::renderInterval() const
{
    return m_renderMs;
}

// ---- 逻辑 timer 控制 ----
void JZGameView::restartLogic()
{
    m_errMsg.clear();
    if (m_logicTimer)
        m_logicTimer->start(m_logicMs, m_maxCatchUp);
}

void JZGameView::stopLogic()
{
    if (m_logicTimer)
        m_logicTimer->stop();
}

bool JZGameView::logicActive() const
{
    return m_logicTimer && m_logicTimer->isActive();
}

const std::string& JZGameView::errorMessage() const
{
    return m_errMsg;
}

// ---- 键盘 ----
bool JZGameView::isKeyDown(uint32_t vk) const
{
    return m_keys.count(vk) > 0;
}

const std::set<uint32_t>& JZGameView::keysDown() const
{
    return m_keys;
}

// ---- 事件 ----
void JZGameView::showEvent(JZShowEvent* event)
{
    JZWidget::showEvent(event);
    auto now = std::chrono::steady_clock::now();
    m_lastFpsTick = now;
    m_animTick = now;
    resetGame();      // 此时 width()/height() 已就绪 (子类在这里按当前尺寸布阵)
    setFocus();       // 抢占焦点以接收键盘事件
    m_logicTimer->start(m_logicMs, m_maxCatchUp);   // 逻辑: 固定步长
    m_renderTimer->start(m_renderMs);               // 渲染: 默认尽可能快
}

void JZGameView::paintEvent(JZPainterEvent* event)
{
    (void)event;
    // 框架约定: paintEvent 内自建画笔 (裁剪由 painter 构造设定), 事件只提供重绘区域
    JZPainter painter(this);
    renderGame(&painter);
}

void JZGameView::keyPressEvent(JZKeyEvent* event)
{
    m_keys.insert(event->key());
    event->accept();
}

void JZGameView::keyReleaseEvent(JZKeyEvent* event)
{
    m_keys.erase(event->key());
    event->accept();
}

// ---- timer 回调 ----
void JZGameView::onLogicTick()
{
    stepGame();
}

void JZGameView::onRenderTick()
{
    auto now = std::chrono::steady_clock::now();

    // 动画时钟: 与逻辑步长解耦, 渲染期推进 (旋转/缩放等变换的演示都用它)
    m_animDt = std::chrono::duration<double>(now - m_animTick).count();
    m_animTick = now;
    m_animT += m_animDt;

    // FPS 统计 (指数加权 + 峰值)
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastFpsTick).count();
    m_lastFpsTick = now;
    if (ms > 0) {
        float f = 1000.0f / (float)ms;
        m_fps = m_fps == 0 ? f : (m_fps * 0.9f + f * 0.1f);
        if (f > m_peakFps) m_peakFps = f;
    }

    update();   // 触发重绘
}

void JZGameView::onLogicErrorSlot(int missed)
{
    onFallBehind(missed);
}

// 逻辑落后过多 (补不过来): 默认停止游戏并记下错误信息, 不再追帧。
// 子类可重写 (如换自己的遮罩/自动重开), 但通常应保留 stopLogic 语义。
void JZGameView::onFallBehind(int missed)
{
    jzWarning() << "JZGameView logic fell too far behind (" << missed
                << " frames missed), stopping game";
    if (m_logicTimer)
        m_logicTimer->stop();   // JZFixedTimer 在 emit sigError 前已 stop, 这里双保险
    m_errMsg = "logic fell too far behind (" + std::to_string(missed) + " frames missed)";
    update();   // 立即重绘以显示错误遮罩
}
