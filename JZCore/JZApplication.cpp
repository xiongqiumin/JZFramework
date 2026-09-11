#include <thread>
#include <algorithm>
#include <assert.h>
#include "JZApplication.h"
#include "JZStd.h"
#include "JZDir.h"
#include "JZPlatform.h"
#include "JZDebug.h"
#include "JZTx.h"
#include "JZDriver.h"
#include "JZWindow.h"
#ifdef HAS_JZWidget
#include "JZStyleSheetParser.h"
#endif

#ifdef _WIN32
#include "JZWin32Thread.h"
#endif

JZApplication *jzApp = nullptr;

JZApplication* JZApplication::instance()
{
    return jzApp;
}

JZApplication::JZApplication()
    :JZObject(JZObject::MainThreadInit())
{
    JZ_ASSERT(!jzApp);
    jzApp = this;

    m_returnCode = 0;

#ifdef _WIN32
    m_mainThread = new JZWin32Thread();
#else
    m_mainThread = new JZThread(JZObject::MainThreadInit());
#endif
    m_mainThread->attachStdThread(JZThread::currentThreadId());
    m_mainThread->moveToThread(m_mainThread);
    m_mainThread->setObjectName("main_thread");
    m_mainThread->m_running = true;

    m_thread = m_mainThread;

    JZDriver::init();
}

JZApplication::~JZApplication()
{
    JZDriver::deinit();

    std::vector<JZObject*> childs = m_childs;
    for (int i = 0; i < (int)childs.size(); i++)
        delete childs[i];
    m_childs.clear();

    m_thread = nullptr;

    m_mainThread->m_running = false;
    m_mainThread->detachStdThread();
    vec_removeAll(m_threads, m_mainThread);
    JZ_ASSERT(m_threads.empty());

    delete m_mainThread;
    jzApp = nullptr;

#ifdef HAS_JZWidget
    if (m_styleSheet)
    {
        delete m_styleSheet;
        m_styleSheet = nullptr;
    }
#endif
}

std::string JZApplication::applicationFilePath()
{
    return JZPlatform::applicationFilePath();
}

std::string JZApplication::applicationDirPath()
{
    return JZPlatform::applicationDirPath();
}

std::string JZApplication::workingDirPath() const
{
    return JZDir::currentPath();
}

JZThread *JZApplication::mainThread()
{
    return m_mainThread;
}

JZThread *JZApplication::currentThread()
{
    JZMutexLocker lock(&m_mutex);
    auto id = JZThread::currentThreadId();
    for (int i = 0; i < (int)m_threads.size(); i++)
    {
        if (m_threads[i]->id() == id)
            return m_threads[i];
    }
    JZ_ASSERT(0);
    return nullptr;
}

void JZApplication::addThread(JZThread *thread)
{
    JZMutexLocker lock(&m_mutex);
    m_threads.push_back(thread);
}

void JZApplication::removeThread(JZThread *thread)
{
    JZMutexLocker lock(&m_mutex);
    vec_removeAll(m_threads, thread);
}

void JZApplication::globalLock()
{
    m_mutex.lock();
}

void JZApplication::globalUnlock()
{
    m_mutex.unlock();
}

int JZApplication::exec()
{
    JZTX_FUNC_TRACE

    // virtual dispatch: JZWin32Thread::run on Windows (standard message loop)
    m_mainThread->run();
    return m_returnCode;
}

void JZApplication::quit()
{
    m_mainThread->quit();
}

void JZApplication::exit(int code)
{
    m_returnCode = code;
    m_mainThread->quit();
}

void JZApplication::processEvents()
{
    JZTX_FUNC_TRACE
    currentThread()->processEvents();
}

void JZApplication::timerEvent(JZTimerEvent *event)
{
}

void JZApplication::sendEvent(JZObject *object, JZEvent *event)
{
    if (object->thread()->id() == JZThread::currentThreadId())
        object->thread()->notify(object, event);
    else
    {
        postEventAndWait(object, event);        
    }
}

void JZApplication::postEvent(JZObject *object, JZEvent *event)
{
    JZEventInfo e(object, event);
    object->thread()->postEvent(e);
}

void JZApplication::postEventAndWait(JZObject *object, JZEvent *event)
{
    JZSemaphore sem;
    
    JZEventInfo e(object, event);
    e.sem = &sem;
    object->thread()->postEvent(e);

    sem.acquire();
}


// ==================== 顶层窗口 ====================
// 实现放在本文件而不是拆到 JZWidget: JZCore 的编译单元本就能看到 JZWindow
// (JZWin32Thread 亦如此), 不必为一个列表再多一层转发。
#ifdef HAS_JZWidget
std::vector<JZWindow*> JZApplication::topLevelWindows() const
{
    return m_topLevelWindows;
}

JZWindow* JZApplication::activeWindow()
{
    for (auto* w : m_topLevelWindows) {
        if (w->isVisible() && w->isActiveWindow())
            return w;
    }
    // 应用整体失焦: 退回最近激活过、且仍存活可见的窗口
    if (m_activeWindow &&
        std::find(m_topLevelWindows.begin(), m_topLevelWindows.end(), m_activeWindow)
        != m_topLevelWindows.end() &&
        m_activeWindow->isVisible() && m_activeWindow->nativeHandle())
        return m_activeWindow;
    return nullptr;
}

void JZApplication::setActiveWindow(JZWindow* w)
{
    m_activeWindow = w;
}

void JZApplication::addTopLevelWindow(JZWindow* w)
{
    if (!w)
        return;
    // 幂等: 重复登记会让"最后一个窗口关闭"的判定失真
    if (std::find(m_topLevelWindows.begin(), m_topLevelWindows.end(), w)
        != m_topLevelWindows.end())
        return;
    m_topLevelWindows.push_back(w);
}

void JZApplication::removeTopLevelWindow(JZWindow* w)
{
    m_topLevelWindows.erase(
        std::remove(m_topLevelWindows.begin(), m_topLevelWindows.end(), w),
        m_topLevelWindows.end());
    if (m_activeWindow == w)
        m_activeWindow = nullptr;
}

// 原生窗口被销毁 (WM_DESTROY) 时由 JZWindow 调用。
// 判定"还有可见窗口"而不是"还有窗口对象": 只关闭、未销毁的对象 (如 hide 的
// JZDialog 可复用) 不再算存活, 否则关掉主窗口后程序永远退不出去。
void JZApplication::notifyWindowClosed(JZWindow*)
{
    if (!m_quitOnLastWindowClosed)
        return;
    for (auto* w : m_topLevelWindows) {
        if (w->isVisible() && w->nativeHandle())
            return;                 // 还有可见的顶层窗口
    }
    quit();
    // Win32 主循环阻塞在 GetMessage: quit() 只置标志 + notify 条件变量, 唤醒不了
    // 消息泵 (最长要等一个心跳周期)。补一次 wakeUp 让退出立即生效。
    m_mainThread->wakeUp();
}

// ==================== 全局样式表 ====================

bool JZApplication::setStyleSheet(const std::string& css)
{
    if (!m_styleSheet)
        m_styleSheet = new JZStyleSheetParser();

    if (!m_styleSheet->parse(css))
        return false;   // 解析失败: 保持上一次成功的内容, 原因见 styleSheetError()

    m_styleSheetText = css;

    // 整表替换: 所有顶层窗口整树重新解析样式并重绘
    // (变量在 parse 期已替换成字面量, 故换主题只能靠换表 —— 无变量继承/作用域覆盖)
    for (size_t i = 0; i < m_topLevelWindows.size(); i++)
    {
        if (m_topLevelWindows[i])
            m_topLevelWindows[i]->styleChanged();
    }
    return true;
}

std::string JZApplication::styleSheet() const
{
    return m_styleSheetText;
}

std::string JZApplication::styleSheetError() const
{
    return m_styleSheet ? m_styleSheet->errorString() : std::string();
}

const JZStyleSheetParser* JZApplication::styleSheetParser() const
{
    return m_styleSheet;
}
#endif