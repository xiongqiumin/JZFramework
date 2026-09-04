#ifndef JZ_APPLICATION_H_
#define JZ_APPLICATION_H_

#include <vector>
#include "JZObject.h"
#include "JZEventLoop.h"
#include "JZThread.h"
#include "JZMutex.h"

class JZWindow;     // 前视声明: 定义在 JZWidget, 这里只用指针, 不引入模块依赖

class JZApplication : public JZObject
{
public:
    static JZApplication* instance();
    
    JZApplication();
    ~JZApplication();    

    std::string applicationFilePath();
    std::string applicationDirPath();

    JZThread *mainThread();
    JZThread *currentThread();

    void addThread(JZThread *thread);
    void removeThread(JZThread *thread);

    void globalLock();
    void globalUnlock();
    
    int exec();        
    void quit();
    void exit(int code);
    void processEvents();

#ifdef HAS_JZWidget
    // ---- 顶层窗口 (JZWindow) ----
    // 列表由 JZWindow 构造/析构自动登记, 按创建顺序排列。
    std::vector<JZWindow*> topLevelWindows() const;

    // 当前活动窗口: 优先返回持有键盘焦点的那个; 应用整体失焦 (用户切到别的程序)
    // 时退回"最近激活过、且仍然可见"的窗口 (语义同 Qt)。都没有则返回 nullptr。
    JZWindow* activeWindow();

    // 记录"最近激活的窗口", 由 JZWindow 获得焦点时调用; 外部一般不需要直接调。
    void setActiveWindow(JZWindow* w);

    // 最后一个可见顶层窗口关闭时自动退出事件循环 (默认开, 等价 Qt 的
    // quitOnLastWindowClosed)。关掉后必须自行调 quit() 退出。
    void setQuitOnLastWindowClosed(bool on) { m_quitOnLastWindowClosed = on; }
    bool quitOnLastWindowClosed() const { return m_quitOnLastWindowClosed; }

    // 由 JZWindow 调用: 构造登记 / 析构注销 / 原生窗口被销毁时通知 (可能触发 quit)
    void addTopLevelWindow(JZWindow* w);
    void removeTopLevelWindow(JZWindow* w);
    void notifyWindowClosed(JZWindow* w);
#endif

    // 同步派发: 同线程经目标线程 notify (先过对象事件过滤器链, 拦截则不派发);
    // 跨线程投递到目标线程并阻塞等待处理完。
    void sendEvent(JZObject *object, JZEvent *event);
    // 异步投递: 立即返回, 目标线程事件循环取出后同样经 notify/过滤器。
    void postEvent(JZObject *object, JZEvent *event);    

protected:    
    virtual void timerEvent(JZTimerEvent *event) override;
    void postEventAndWait(JZObject *object, JZEvent *event);
    
    JZThread *m_mainThread;

    std::vector<JZThread*> m_threads;
    JZMutex m_mutex;
    int m_returnCode;

#ifdef HAS_JZWidget
    // 顶层窗口 (由 JZWindow 构造/析构维护)
    std::vector<JZWindow*> m_topLevelWindows;
    JZWindow* m_activeWindow = nullptr;     // 最近激活过的窗口 (activeWindow 的回退值)
    bool m_quitOnLastWindowClosed = true;
#endif
};
extern JZApplication *jzApp;

#endif


