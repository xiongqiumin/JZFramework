#ifndef JZ_VIRTUAL_WINDOW_BACKEND_H_
#define JZ_VIRTUAL_WINDOW_BACKEND_H_
#include "JZWindowBackend.h"

#include <atomic>

class JZVirtualWindow;

// ==================== 虚拟桌面窗口后端 ====================
// "另一个平台"的后端实现: 挂在**客户区那个真 JZWindow** 上, 把它的平台动作落到
// JZVirtualWindow (虚拟侧的窗口对象) 与 JZVirtualDevice (虚拟显示器) 上:
//   · handle()  -> 虚拟侧的窗口对象 (于是 JZWindow::nativeHandle() 在虚拟桌面上也有值);
//   · create    -> 建离屏帧缓冲 (而不是 CreateWindowExW);
//   · show      -> 出首帧并让桌面重新合成 (而不是 ShowWindow);
//   · 移动/激活  -> 改桌面坐标 / 桌面管理器 raise + 焦点 (而不是 SetWindowPos/SetForegroundWindow);
//   · 取帧      -> 读自己的帧缓冲 (而不是抓屏);
//   · 输入法 / 系统光标 / 鼠标抓取 -> 没有 (空实现) —— 真窗口才有的东西。
// ⚠ 框架语义 (焦点控件/悬停/输入蒙版/关闭事件/事件分发) 全在 JZWindow —— 所以
//   "虚拟窗口与真实窗口行为一致"是结构上保证的, 不是靠两边各写一遍对齐。
class JZVirtualWindowBackend : public JZWindowBackend
{
public:
    explicit JZVirtualWindowBackend(JZVirtualWindow* vwin);
    ~JZVirtualWindowBackend() override;

    JZVirtualWindow* window() const { return m_vwin; }

    bool create(JZWindow* w) override;
    void destroy(JZWindow* w) override;
    void onPlatformDestroyed(JZWindow* w) override;
    bool isCreated() const override { return m_created; }
    void* handle() const override;      // 虚拟侧的窗口对象 (唯一定义在 .cpp)

    void show(JZWindow* w) override;
    void hide(JZWindow* w) override;
    void setTitle(JZWindow* w, const std::string& title) override;
    void setClientSize(JZWindow* w, int cw, int ch) override;
    void setScreenPos(JZWindow* w, int x, int y) override;

    JZPoint clientToScreen(JZWindow* w, const JZPoint& pt) const override;
    JZPoint screenToClient(JZWindow* w, const JZPoint& pt) const override;
    float deviceScale(JZWindow* w) const override;
    bool containsScreenPoint(JZWindow* w, int x, int y) const override;

    // 脏区: **发到窗口所属线程**去画 —— 跨线程的窗口 (虚拟进程建的) 发到它自己的进程
    // 线程, 由那边的**事件循环**排掉 (虚拟进程也是一个 JZThread, 有自己的循环);
    // 与宿主同线程的窗口就按框架默认, 排进主线程的 paint task。两者都不"就地画"。
    void onInvalidate(JZWindow* w, const JZRect& rc) override;
    // 画完把这一帧推给桌面 (拷贝 + 通知重合成) —— 帧缓冲的读写都在窗口自己的线程上
    void onPainted(JZWindow* w) override;

    void setTop(JZWindow* w, bool on) override;
    void showMinimized(JZWindow* w) override;
    void showMaximized(JZWindow* w) override;
    void showNormal(JZWindow* w) override;
    void activate(JZWindow* w) override;
    void updateCursor(JZWindow* w) override;         // 没有系统光标
    void setCapture(JZWindow* w, bool on) override;  // 桌面不做系统抓取

    // 输入法: 虚拟窗口没有 IMC (组不了字), 但"要不要输入法"要**回给桌面** ——
    // 宿主那扇真窗口据此给 IME 开关 UI, 并把组字消息转投进来 (见 JZVirtualScreenView)。
    // 判断本身不自造: 这两个钩子就是 JZWindow::updateImeContext 的结论。
    void imeAssociate(JZWindow* w) override;
    void imeUpdateUi(JZWindow* w, bool show) override;
    void imePrepare(JZWindow* w, int fontPixelSize) override;   // 组字窗字体: 虚拟侧无事可做
    void imeSetPosition(JZWindow* w, const JZRect& lineRect, const JZPoint& candPos) override;

    int    execModal(JZWindow* w, bool disableOwner) override;   // 没有平台模态循环
    JZImage grabClient(JZWindow* w) override;        // 读它自己的帧缓冲

private:
    float deviceScaleOf() const;         // 本窗口该用的渲染缩放 (== 桌面的 scale)

    JZVirtualWindow* m_vwin = nullptr;
    bool m_created = false;              // 离屏帧缓冲是否已建
    // 脏区合并 (见 onInvalidate): "已有一帧排在窗口线程上" + 排它的时刻。
    // ⚠ 光有布尔量不行 —— 它只要有一次投递没落地就会**永远**停在"已排"上, 窗口之后再也
    //   不重绘(实测: 游戏窗口 FPS 掉到 5-6、画面冻在最后一帧, 桌面合成缓存跟着滞后数秒)。
    //   所以配一个时刻: 排出去太久还没画完就认为那一次丢了, 允许再排(自愈)。
    std::atomic<bool> m_paintPending{ false };
    std::atomic<int64_t> m_paintReqMs{ 0 };
};

#endif // JZ_VIRTUAL_WINDOW_BACKEND_H_
