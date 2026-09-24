#ifndef JZ_JS_HANDLE_REF_H_
#define JZ_JS_HANDLE_REF_H_

#include "JZJSEngine.h"

// =========================================================================
// 句柄"是否算一票让宿主活着" —— 对齐 Node 的 handle ref 语义
//
//   setLive(true/false)   打开/监听/连接 <-> 关闭/断开/析构
//   setRef(true/false)    ref()/unref() —— 只改"算不算一票", **不影响句柄功能**
//                         (unref 之后照常收发, 只是不再吊住宿主)
//
// 内部保证与 engine->addPendingWork()/releasePendingWork() **严格配对**(counted):
// 重复调用、先 unref 后 live、live 两次…计数都不会跑偏。
// ⚠ unref 在"还没 live"时调用也有效 —— 状态留着, 等它 live 时按 refed 决定要不要计票。
//
// 用法(net/dgram/ws/mqtt/http socket、modbus/serialport 连接、worker 句柄):
//   成员: JZJSHandleRef href;  构造时 href.attach(engine);
//   打开处 href.setLive(true);  关闭/断开/析构处 href.setLive(false);
//   JS 面 ref()/unref() 直接转 href.setRef(true/false)。
// =========================================================================
struct JZJSHandleRef
{
    JZJSEngine* engine = nullptr;
    bool live = false;      // 句柄是否处于打开态
    bool refed = true;      // 是否算一票(默认算, 同 Node)
    bool counted = false;   // 当前是否已计入引擎 pendingWork

    void attach(JZJSEngine* e) { engine = e; }
    // 先撤票再断开引擎引用(引擎销毁路径用, 避免留下悬垂 engine)
    void detach()
    {
        setLive(false);
        engine = nullptr;
    }

    bool hasRef() const { return live && refed; }

    void setLive(bool v)
    {
        live = v;
        sync();
    }

    void setRef(bool v)
    {
        refed = v;
        sync();
    }

    void sync()
    {
        bool want = live && refed;      // 只有"活着且 ref"才计一票
        if (want == counted || !engine)
            return;

        if (want)
            engine->addPendingWork();
        else
            engine->releasePendingWork();
        counted = want;
    }
};

#endif  // !JZ_JS_HANDLE_REF_H_
