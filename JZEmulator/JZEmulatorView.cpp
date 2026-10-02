// ⚠ windows.h 要在最前（有保护宏）：框架/OpenCV 那类头会带进 windows.h
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#endif

#include "JZEmulatorView.h"

#include "JZColor.h"
#include "JZDebug.h"          // jzWarning：帧节拍落后过多时上报
#include "JZFixedTimer.h"
#include "JZKeyEvent.h"
#include "JZPainter.h"
#include "JZPainterEvent.h"
#include "JZShowEvent.h"
#include "JZSignal.h"

namespace
{
    const int kTickMs = 16;      // ~60Hz（GBA 实际是 59.7Hz）

    // 单次落后最多补多少帧；超过就认为系统真跟不上了（JZFixedTimer 会自己 stop + sigError）。
    // 30 帧 ≈ 0.5 秒：我们每帧成本只有 1~2ms（模拟 0.13ms + 转换 + 直拷贴图），16ms 的预算里
    // 余量很大，偶发卡顿完全补得回来。
    // ⚠ 别用 JZFixedTimer 的默认值 5 —— 宿主随便一次长卡顿（别的窗口拖拽、调试点、GC）
    //   就可能超过 5 帧，那会让游戏**直接停住**，看起来像挂了。
    const int kMaxCatchUp = 30;

    // 键码 = Win32 VK_*（与 JZKeyEvent::key() 同一套）。
    // 映射沿用 E:/work/simulator/host/demo/main.c 那一套，另补 GBA 的 L/R。
    const int kVkReturn = 0x0D;
    const int kVkShift  = 0x10;
    const int kVkLeft   = 0x25;
    const int kVkUp     = 0x26;
    const int kVkRight  = 0x27;
    const int kVkDown   = 0x28;
}

JZEmulatorView::JZEmulatorView(JZWidget* parent)
    : JZWidget(parent)
{
    // parent = this ⇒ 归本控件所有。这里用 JZFixedTimer（墙钟补偿）而不是 JZTimer，
    // 理由见头文件"帧驱动"那段。
    m_timer = new JZFixedTimer(this);
    connect(&m_timer->sigTimeout, this, &JZEmulatorView::onTick);
    connect(&m_timer->sigError, this, &JZEmulatorView::onTimerError);
}

JZEmulatorView::~JZEmulatorView()
{
    stop();
    m_core.unload();                  // 先卸 DLL 再让定时器/控件父子析构（顺序无碍，但显式点）
}

// ==================== 装配 ====================
bool JZEmulatorView::openRom(const std::string& dllDir, const std::string& romPath)
{
    if (!m_core.load(dllDir))
        return false;
    if (!m_core.openRom(romPath))
        return false;
    m_frames = 0;
    return true;
}

void JZEmulatorView::start(int intervalMs)
{
    if (!m_timer)
        return;
    m_timer->start(intervalMs > 0 ? intervalMs : kTickMs, kMaxCatchUp);
}

void JZEmulatorView::stop()
{
    if (m_timer)
        m_timer->stop();
    m_pressed.clear();                // 停表即松手：免得下次开跑时带着一个卡住的键
    m_core.setKeys(0);
}

bool JZEmulatorView::isRunning() const
{
    return m_timer && m_timer->isActive();
}

// ==================== 键盘 ====================
uint32_t JZEmulatorView::gbaKeyOf(int vk)
{
    switch (vk)
    {
    case kVkLeft:   return JZEmulatorCore::KeyLeft;
    case kVkRight:  return JZEmulatorCore::KeyRight;
    case kVkUp:     return JZEmulatorCore::KeyUp;
    case kVkDown:   return JZEmulatorCore::KeyDown;
    case 'Z':       return JZEmulatorCore::KeyA;
    case 'X':       return JZEmulatorCore::KeyB;
    case kVkReturn: return JZEmulatorCore::KeyStart;
    case kVkShift:  return JZEmulatorCore::KeySelect;
    case 'A':       return JZEmulatorCore::KeyL;
    case 'S':       return JZEmulatorCore::KeyR;
    default:        return 0;
    }
}

void JZEmulatorView::syncKeys()
{
    uint32_t keys = 0;
    if (hasFocus())
    {
        for (std::set<int>::const_iterator it = m_pressed.begin(); it != m_pressed.end(); ++it)
            keys |= gbaKeyOf(*it);
    }
    m_core.setKeys(keys);
}

void JZEmulatorView::keyPressEvent(JZKeyEvent* event)
{
    if (!event)
        return;
    // 长按会反复来 keyPress：用 set 去重，并按"第一次按下"立刻下发。
    // ⚠ 还要处理"这一帧内又被重新按下"（见 keyReleaseEvent 的延后松开）：把它从延后表里摘掉。
    const int vk = event->key();
    const bool fresh = m_pressed.insert(vk).second;
    if (fresh || m_lateRelease.erase(vk) > 0)
    {
        m_pressFrame[vk] = m_frames;
        syncKeys();
    }
    event->accept();
}

void JZEmulatorView::keyReleaseEvent(JZKeyEvent* event)
{
    if (!event)
        return;
    // ---- "至少压住一帧" ----
    // 程序注入的"点一下"是 press+release 一瞬间（算 computer use 的 key()、容器的
    // /api/input/key 都是这样）。若按下之后**一帧都还没跑**就松开，mGBA 眼里那一按从未发生
    // （实测：脚本按 Start 想进菜单，画面毫无反应）。所以这种"同一帧内被松开"的键记进
    // m_lateRelease，等下一帧带着它跑完再真松开（见 onTick）。
    const int vk = event->key();
    if (m_pressed.find(vk) != m_pressed.end())
    {
        if (m_pressFrame[vk] >= m_frames)
            m_lateRelease.insert(vk);       // 这一帧还没跑过 ⇒ 延后到跑完那一帧
        else
        {
            m_pressed.erase(vk);
            m_pressFrame.erase(vk);
            syncKeys();
        }
    }
    event->accept();
}

void JZEmulatorView::showEvent(JZShowEvent* event)
{
    JZWidget::showEvent(event);
    setFocus();      // 抢键盘焦点：不然按键先被别的控件/窗口吃掉
}

// ==================== 帧 ====================
void JZEmulatorView::onTimerError(int missed)
{
    // JZFixedTimer 在"单次落后超过 maxCatchUp 帧"时会自己 stop() 并通过 sigError 上报
    // （防死亡螺旋：真跟不上时，补帧只会越补越落后）。这里只报出来、不自动重启 ——
    // 让宿主/驱动决定是再 start() 一次（继续追），还是接受（游戏暂停在这一刻）。
    jzWarning() << "[JZEmulatorView] 帧节拍落后 " << missed << " 帧，已停止补帧（游戏暂停）";
}

void JZEmulatorView::onTick()
{
    if (!m_core.isRomOpen())
        return;

    syncKeys();          // 含"失焦 -> 全部松开"这条
    m_core.runFrame();

    // 延后松开的那批键：这一帧已经**带着它们**跑过了，现在才真松开（见 keyReleaseEvent）。
    // ⚠ 顺序不能反：先 syncKeys()（键按下）→ 再 runFrame()（模拟器吃到这一按）→ 才松开。
    if (!m_lateRelease.empty())
    {
        for (std::set<int>::const_iterator it = m_lateRelease.begin(); it != m_lateRelease.end(); ++it)
        {
            m_pressed.erase(*it);
            m_pressFrame.erase(*it);
        }
        m_lateRelease.clear();
        syncKeys();
    }

    int w = 0, h = 0;
    const uint8_t* src = m_core.frame(&w, &h);
    if (src && w > 0 && h > 0)
    {
        const size_t n = (size_t)w * h;
        if (m_rgba.size() < n * 4)
            m_rgba.resize(n * 4);

        // 宿主 DLL 给的每像素 4 字节是 B,G,R,X —— 第 4 个字节**不是 alpha**
        // （实测：38400 个像素里没有一个等于 0 或 255，是 88 之类的元信息，
        //  所以既不能当透明通道用，也不能就地改写它：那里可能有模拟器要用的数据）。
        // JZImage 要的是 RGBA8888 ⇒ 在这里交换 R/B 并把 alpha 填满。
        {
            const uint8_t* s = src;
            uint8_t* d = m_rgba.data();
            for (size_t i = 0; i < n; ++i, s += 4, d += 4)
            {
                d[0] = s[2];   // R
                d[1] = s[1];   // G
                d[2] = s[0];   // B
                d[3] = 255;    // A
            }
        }

        // 1:1 上屏（不放大、不超分）：贴图那一步源/目标尺寸相等 ⇒ 后端走直拷，又快又不糊。
        // ⚠ 必须每帧换一个**新的 JZImageData**（fromRGBA 内部新建）。
        //   不能复用同一个 data 改像素：GDI 后端把 JZImage 转成位图后是按
        //   JZImageData* 指针缓存的，复用同一份会永远贴出第一帧。
        m_image = JZImage::fromRGBA(m_rgba.data(), w, h);
    }

    ++m_frames;
    update();            // 标脏；真正的重绘由帧循环合并后调 paintEvent
}

void JZEmulatorView::paintEvent(JZPainterEvent* event)
{
    (void)event;
    JZPainter p(this);                       // 框架约定：paintEvent 内自建画笔
    const int cw = width(), ch = height();
    p.fillRect(0, 0, cw, ch, JZColor(0, 0, 0));   // 先铺黑（顺带当黑边）
    if (m_image.isNull() || cw <= 0 || ch <= 0)
        return;

    // 1:1 居中：控件比画面大就四周留黑边（不拉伸、不进插值 ⇒ 不糊；源/目标同尺寸还走直拷）
    const int dw = m_image.width(), dh = m_image.height();
    p.drawImage(m_image, (cw - dw) / 2, (ch - dh) / 2, dw, dh);
}
