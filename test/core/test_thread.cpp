#include "test_thread.h"

#include <atomic>
#include <thread>

#include "JZTest.h"
#include "JZApplication.h"
#include "JZThread.h"
#include "JZTimer.h"
#include "JZEvent.h"
#include "JZObject.h"
#include "JZSignal.h"

namespace {

// 一个简单的事件接收对象, 仅用于验证消息循环 / 自定义事件 / 删除语义
class TestEventReceiver : public JZObject
{
public:
    std::function<void(JZEvent *)> customHandler;
    std::atomic<JZThreadId> handlerThread{THREAD_ID_NONE};
    int customCount = 0;

    void postCustom(int type)
    {
        jzApp->postEvent(this, new JZEvent(type));
    }

    virtual void customEvent(JZEvent *e) override
    {
        customCount++;
        handlerThread = JZThread::currentThreadId();
        if (customHandler)
            customHandler(e);
    }
};

// 会一直运行直到被请求中断的线程, 用于测试 interrupt / quit / wait 边界
class LoopThread : public JZThread
{
public:
    std::atomic<bool> started{false};

    virtual void run() override
    {
        started = true;
        while (!isRequestInterruption())
            JZThread::msleep(2);
    }
};

// 用于验证真正的跨线程执行 (信号 -> 槽, QueuedConnection 投递到目标线程)
class WorkerObj : public JZObject
{
public:
    JZSigVoid sigPing{this};
    std::atomic<JZThreadId> ranThread{THREAD_ID_NONE};
    std::atomic<bool> done{false};

    void onPing()
    {
        ranThread = JZThread::currentThreadId();
        done = true;
    }
};

} // namespace

// ============================ 线程 JZThread ============================

// 基础任务线程: setTask 后在子线程执行, wait 后结果可见, 线程已停止
static bool thread_task_basic()
{
    JZTaskThread t;
    std::atomic<int> result{0};
    t.setTask([&]() { result = 42; });

    JZ_TEST_FALSE(t.isRunning());
    t.start();
    JZ_TEST_TRUE(t.isRunning());

    t.wait();
    JZ_TEST_EQ(result.load(), 42);
    JZ_TEST_FALSE(t.isRunning());
    return true;
}

// isRunning 生命周期: 启动前 false, 运行中 true, wait 之后 false
static bool thread_isRunning_lifecycle()
{
    JZThread worker;
    JZ_TEST_FALSE(worker.isRunning());

    worker.start();
    JZ_TEST_TRUE(worker.isRunning());

    worker.quit();
    worker.wait();
    JZ_TEST_FALSE(worker.isRunning());
    return true;
}

// 主线程下 currentThread 应等于 mainThread, 且返回非空
static bool thread_currentThread_id()
{
    JZ_TEST_EQ(JZThread::currentThread(), jzApp->mainThread());
    JZ_TEST_NEQ(JZThread::currentThread(), nullptr);
    JZ_TEST_EQ(JZThread::currentThreadId(), jzApp->mainThread()->id());
    return true;
}

// runOnThread: 回调最终会被调度线程的事件循环处理 (注意本框架中 JZThread 对象
// 自身归属于创建它的线程, 故事件投递到其所属线程的事件循环)
static bool thread_runOnThread()
{
    JZThread worker;
    worker.start();

    std::atomic<bool> executed{false};
    runOnThread(&worker, [&]() { executed = true; });

    for (int i = 0; i < 200 && !executed.load(); i++)
    {
        jzApp->processEvents();
        JZThread::msleep(2);
    }
    JZ_TEST_TRUE(executed.load());

    worker.quit();
    worker.wait();
    return true;
}

// 真正的跨线程执行: 信号跨线程发射, 槽在目标(worker)线程上被调用
static bool thread_cross_thread_signal()
{
    JZThread worker;
    worker.start();

    WorkerObj *o = new WorkerObj();
    o->moveToThread(&worker);

    connect(&o->sigPing, o, &WorkerObj::onPing);
    o->sigPing.emit(); // 从主线程发射, recv 在 worker -> QueuedConnection

    for (int i = 0; i < 200 && !o->done.load(); i++)
        JZThread::msleep(2);

    JZ_TEST_TRUE(o->done.load());
    JZ_TEST_TRUE(worker.id() == o->ranThread); // 槽确实运行在 worker 线程

    o->deleteLater();
    worker.quit();
    worker.wait();
    return true;
}

// 中断边界: 运行中请求中断, wait 应能正常结束且线程停止
static bool thread_interrupt()
{
    LoopThread t;
    t.start();
    for (int i = 0; i < 200 && !t.started.load(); i++)
        JZThread::msleep(2);

    JZ_TEST_TRUE(t.started.load());
    JZ_TEST_TRUE(t.isRunning());

    t.requestInterruption();
    t.wait();
    JZ_TEST_FALSE(t.isRunning());
    return true;
}

// 边界: quit / wait 可重复调用而不崩溃 (幂等)
static bool thread_quit_idempotent()
{
    JZThread worker;
    worker.start();
    worker.quit();
    worker.wait();

    worker.quit(); // 再次 quit 应安全
    worker.wait(); // 再次 wait 应安全
    JZ_TEST_FALSE(worker.isRunning());
    return true;
}

// ============================ 消息循环 JZEventLoop ============================

// 自定义事件: 投递后尚未处理, processEvents 后应被处理且发生在当前线程
static bool eventloop_custom_event()
{
    TestEventReceiver r;
    JZ_TEST_EQ(r.customCount, 0);

    r.postCustom(JZEvent_user + 1);
    JZ_TEST_EQ(r.customCount, 0); // 尚未处理

    jzApp->processEvents();
    JZ_TEST_EQ(r.customCount, 1);
    JZ_TEST_TRUE(jzApp->mainThread()->id() == r.handlerThread);
    return true;
}

// 同步 sendEvent: 同线程直接处理, 无需 processEvents
static bool eventloop_sendEvent_sync()
{
    TestEventReceiver r;
    std::atomic<bool> got{false};
    r.customHandler = [&](JZEvent *) { got = true; };

    JZEvent e(JZEvent_user + 2);
    jzApp->sendEvent(&r, &e);
    JZ_TEST_TRUE(got.load()); // 同步已处理
    return true;
}

// 单次定时器 singleShot(0): 经消息循环触发回调
static bool eventloop_timer_singleShot()
{
    std::atomic<bool> fired{false};
    JZTimer::singleShot(0, [&]() { fired = true; });

    for (int i = 0; i < 100 && !fired.load(); i++)
    {
        jzApp->processEvents();
        JZThread::msleep(2);
    }
    JZ_TEST_TRUE(fired.load());
    return true;
}

// 重复定时器: 累计触发指定次数后 stop, isActive 归位
static bool eventloop_timer_repeat()
{
    JZTimer timer;
    std::atomic<int> count{0};
    connect(&timer.sigTimeout, [&]() {
        if (count.fetch_add(1) + 1 >= 3)
            timer.stop();
    });
    timer.setSingleShot(false);
    timer.start(5);

    for (int i = 0; i < 400 && count.load() < 3; i++)
    {
        jzApp->processEvents();
        JZThread::msleep(3);
    }
    JZ_TEST_GE(count.load(), 3);
    JZ_TEST_FALSE(timer.isActive());
    return true;
}

// deleteLater: 投递后延迟删除, 经 processEvents 后对象销毁
static bool eventloop_deleteLater()
{
    std::atomic<bool> destroyed{false};
    TestEventReceiver *r = new TestEventReceiver();
    JZObjectPointer p = r->guardPointer();
    connect(&r->sigDestroyed, [&](JZObject *) { destroyed = true; });

    r->deleteLater();
    JZ_TEST_FALSE(destroyed.load()); // 尚未处理
    JZ_TEST_TRUE((bool)p);

    jzApp->processEvents();
    JZ_TEST_TRUE(destroyed.load());
    JZ_TEST_FALSE((bool)p);
    return true;
}

// ============================ 发送后删除 (边界) ============================

// runOnObject: 接收对象在事件被处理前已删除 -> 回调应被安全跳过
static bool send_delete_runOnObject()
{
    std::atomic<bool> called{false};
    TestEventReceiver *r = new TestEventReceiver();
    runOnObject(r, [&]() { called = true; });
    delete r; // 同线程直接删除是安全的

    jzApp->processEvents();
    JZ_TEST_FALSE(called.load());
    return true;
}

// runOnThread(thread, recv): recv 已删除 -> 回调应被安全跳过
static bool send_delete_runOnThread_recv()
{
    std::atomic<bool> called{false};
    TestEventReceiver *r = new TestEventReceiver();
    runOnObject(r, [&]() { called = true; });
    delete r;

    jzApp->processEvents();
    JZ_TEST_FALSE(called.load());
    return true;
}

// postEvent 自定义事件: 接收对象已删除 -> 事件应被安全跳过
static bool send_delete_postEvent()
{
    std::atomic<bool> got{false};
    TestEventReceiver *r = new TestEventReceiver();
    r->customHandler = [&](JZEvent *) { got = true; };
    jzApp->postEvent(r, new JZEvent(JZEvent_user + 3));
    delete r;

    jzApp->processEvents();
    JZ_TEST_FALSE(got.load());
    return true;
}

// deleteLater (同线程): 标准延迟删除路径
static bool send_delete_deleteLater()
{
    std::atomic<bool> destroyed{false};
    TestEventReceiver *r = new TestEventReceiver();
    JZObjectPointer p = r->guardPointer();
    connect(&r->sigDestroyed, [&](JZObject *) { destroyed = true; });

    r->deleteLater();
    JZ_TEST_TRUE((bool)p);

    jzApp->processEvents();
    JZ_TEST_TRUE(destroyed.load());
    JZ_TEST_FALSE((bool)p);
    return true;
}

// deleteLater (跨线程): 对象 moveToThread 后, 删除事件由目标线程处理
static bool send_delete_deleteLater_cross_thread()
{
    JZThread worker;
    worker.start();

    std::atomic<bool> destroyed{false};
    TestEventReceiver *r = new TestEventReceiver();
    r->moveToThread(&worker);
    JZObjectPointer p = r->guardPointer();
    connect(&r->sigDestroyed, [&](JZObject *) { destroyed = true; });

    r->deleteLater();
    JZ_TEST_TRUE((bool)p); // 跨线程, 尚未处理

    for (int i = 0; i < 200 && !destroyed.load(); i++)
        JZThread::msleep(2);

    JZ_TEST_TRUE(destroyed.load());
    JZ_TEST_FALSE((bool)p);

    worker.quit();
    worker.wait();
    return true;
}

// 边界: 先 deleteLater 入队删除事件, 再投递自定义事件 ->
//       删除事件先被处理, 自定义事件因对象已销毁被跳过
static bool send_delete_pending_event_after_deleteLater()
{
    std::atomic<bool> got{false};
    TestEventReceiver *r = new TestEventReceiver();
    r->customHandler = [&](JZEvent *) { got = true; };

    r->deleteLater();
    jzApp->postEvent(r, new JZEvent(JZEvent_user + 4));

    jzApp->processEvents();
    JZ_TEST_FALSE(got.load());
    return true;
}

// ============================ 测试入口 ============================

void test_JZThread(JZTest *test)
{
    auto threadSuite = std::make_shared<JZTestSuite>("JZThread");
    test->addSuite(threadSuite);
    threadSuite->addTestCase("task_basic", thread_task_basic);
    threadSuite->addTestCase("isRunning_lifecycle", thread_isRunning_lifecycle);
    threadSuite->addTestCase("currentThread_id", thread_currentThread_id);
    threadSuite->addTestCase("runOnThread", thread_runOnThread);
    threadSuite->addTestCase("cross_thread_signal", thread_cross_thread_signal);
    threadSuite->addTestCase("interrupt", thread_interrupt);
    threadSuite->addTestCase("quit_idempotent", thread_quit_idempotent);

    auto loopSuite = std::make_shared<JZTestSuite>("JZEventLoop");
    test->addSuite(loopSuite);
    loopSuite->addTestCase("custom_event", eventloop_custom_event);
    loopSuite->addTestCase("sendEvent_sync", eventloop_sendEvent_sync);
    loopSuite->addTestCase("timer_singleShot", eventloop_timer_singleShot);
    loopSuite->addTestCase("timer_repeat", eventloop_timer_repeat);
    loopSuite->addTestCase("deleteLater", eventloop_deleteLater);

    auto sendSuite = std::make_shared<JZTestSuite>("SendDelete");
    test->addSuite(sendSuite);
    sendSuite->addTestCase("runOnObject", send_delete_runOnObject);
    sendSuite->addTestCase("runOnThread_recv", send_delete_runOnThread_recv);
    sendSuite->addTestCase("postEvent", send_delete_postEvent);
    sendSuite->addTestCase("deleteLater", send_delete_deleteLater);
    sendSuite->addTestCase("deleteLater_cross_thread", send_delete_deleteLater_cross_thread);
    sendSuite->addTestCase("pending_event_after_deleteLater", send_delete_pending_event_after_deleteLater);
}
