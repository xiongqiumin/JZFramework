#ifndef JZ_WINDOW_BACKEND_H_
#define JZ_WINDOW_BACKEND_H_
#include "JZWidget.h"
#include "JZImage.h"

#include <string>

class JZWindow;

// 脏区排进**主线程**的 paint task (由消息循环统一画一帧; 见 JZWindowBackend::onInvalidate)。
// ⚠ 定义在 JZWin32WindowBackend.cpp: 平台类型 (JZWin32Thread) 只出现在平台实现里,
//   后端接口与框架代码都看不见它。
JZFRAMEWORK_API void jzSchedulePaintOnMainThread(JZWindow* w, const JZRect& rc);

// ==================== 平台窗口后端 ====================
// JZWindow 只持"**一个后端**": 平台相关的动作 (含"平台窗口句柄是什么") 全部从这里走 ——
//   · 真窗口   JZWin32WindowBackend  (HWND / WndProc / SetCapture / IMM / 消息循环)
//   · 虚拟窗口 JZVirtualWindowBackend (见 binary/virtual_device: 句柄 = 虚拟侧的窗口对象)
// 于是"多一个平台" = 多一个后端实现, **JZWindow 一行都不用改**; 而窗口的**行为**
// (控件树/命中/事件分发/焦点/绘制/关闭语义) 仍然只有一份, 不会出现"虚拟窗口与真实
// 窗口行为漂移" —— 这是"同一份脚本可精确重复"的前提。
//
// ⚠ 坐标口径: 一律**屏幕逻辑坐标**(scale=1 的虚拟窗口下即桌面像素)。后端负责
//   clientToScreen / screenToClient, 框架不再自己拼坐标 —— mapToGlobal / 命中测试 /
//   抓图的口径都由它一处决定。
class JZFRAMEWORK_API JZWindowBackend
{
public:
    virtual ~JZWindowBackend() {}

    // ---- 生命周期 ----
    // 惰性创建(首次 show 时由 JZWindow 调一次): 建平台窗口 +
    // 视需要初始化 m_device (真窗口要 init(hwnd,...); 虚拟窗口是离屏帧缓冲)。
    // 返回 false = 没建成(调用方不会把窗口标成"已创建")。
    virtual bool create(JZWindow* w) = 0;
    // 请求销毁平台窗口 (close() 走到这里; 之后 show 会重新 create)
    virtual void destroy(JZWindow* w) = 0;
    // 平台窗口**已经**没了(WM_DESTROY / 桌面收走): 只清后端自己的句柄缓存, 不要再去销毁
    virtual void onPlatformDestroyed(JZWindow* w) = 0;
    virtual bool isCreated() const = 0;
    // 平台窗口句柄: Win32 = HWND (未创建为 nullptr);
    // 虚拟桌面 = 那块桌面上代表本窗口的窗口对象 (见 binary/virtual_device/JZVirtualWindow)。
    // ⚠ 这正是 JZWindow::nativeHandle() 的唯一来源 —— "平台窗口建了没有"由它回答。
    virtual void* handle() const = 0;

    // ---- 显隐 / 标题 / 几何 ----
    virtual void show(JZWindow* w) = 0;
    virtual void hide(JZWindow* w) = 0;
    virtual void setTitle(JZWindow* w, const std::string& title) = 0;
    // 客户区逻辑尺寸变了 (JZWindow::resize 之后)
    virtual void setClientSize(JZWindow* w, int cw, int ch) = 0;
    // 移到屏幕逻辑坐标 (JZWindow::moveScreen)
    virtual void setScreenPos(JZWindow* w, int x, int y) = 0;

    // ---- 坐标换算 (mapToGlobal / 命中测试的口径来源) ----
    virtual JZPoint clientToScreen(JZWindow* w, const JZPoint& pt) const = 0;
    virtual JZPoint screenToClient(JZWindow* w, const JZPoint& pt) const = 0;
    // 本窗口当前所在显示器的缩放 (逻辑像素 -> 物理像素)。真窗口问系统 DPI;
    // 虚拟窗口问它所在的那块桌面 (默认 1 = 纯逻辑桌面; 在宿主界面里显示时, 桌面会同步成
    // 宿主的设备缩放, 见 JZVirtualDevice::setScale / JZVirtualScreenView)。
    // ⚠ 缩放只影响渲染分辨率 (帧缓冲/字体), 不影响坐标: 坐标恒为逻辑像素。
    virtual float deviceScale(JZWindow* w) const = 0;
    // 脏区来了 (JZWindow 收到 invalidate 事件, 见 JZWindow::event): 排给**哪个消息循环**画。
    //   · 真窗口 (默认实现) = 排进主线程的 paint task, 由消息循环统一画 (WM_PAINT 同款);
    //   · 虚拟窗口 = 把脏区**发到窗口所属的虚拟进程线程** (JZVirtualProcess 有自己的事件
    //     循环), 那边的循环画它自己的帧缓冲 —— 见 JZVirtualWindowBackend::onInvalidate。
    // ⚠ 是"排给一个循环", 不是就地画: 帧缓冲的主人只有一个线程 (窗口自己的线程, 见
    //   onPainted), 谁也不该在事件派发栈里替它画 (那是两套绘制时序漂移的来源)。
    virtual void onInvalidate(JZWindow* w, const JZRect& rc) { jzSchedulePaintOnMainThread(w, rc); }

    // 画完一帧后的回调 (本窗口的线程上, 紧接在 render 之后)。
    // 真窗口无事可做(WM_PAINT 后系统自己上屏); 虚拟窗口在这里把新帧**推**给桌面
    // (拷贝 + 通知重合成) —— 于是"帧缓冲的读写都在窗口自己的线程上"。
    virtual void onPainted(JZWindow* w) { (void)w; }
    // 该窗口的屏幕矩形是否包含这个屏幕点 (弹层"外部按下关闭"用; 默认按客户区算,
    // 真窗口可重写成窗口外框 GetWindowRect 的口径)
    virtual bool containsScreenPoint(JZWindow* w, int x, int y) const;

    // ---- 窗口状态 ----
    virtual void setTop(JZWindow* w, bool on) = 0;       // 置顶
    virtual void showMinimized(JZWindow* w) = 0;
    virtual void showMaximized(JZWindow* w) = 0;
    virtual void showNormal(JZWindow* w) = 0;
    virtual void activate(JZWindow* w) = 0;              // 前置 + 争取键盘焦点
    virtual void updateCursor(JZWindow* w) = 0;          // 按当前悬停控件刷新光标形状
    virtual void setCapture(JZWindow* w, bool on) = 0;   // 鼠标抓取 (真窗口 = SetCapture)

    // ---- 模态 / 取帧 ----
    virtual int    execModal(JZWindow* w, bool disableOwner) = 0;
    // 客户区一帧 (逻辑尺寸): 真窗口 = 抓屏裁剪; 虚拟窗口 = 读它自己的帧缓冲
    virtual JZImage grabClient(JZWindow* w) = 0;

    // ---- 输入法 (只有真窗口有: 虚拟窗口这几个默认空实现 = 没有输入法) ----
    virtual void imeAssociate(JZWindow* w) {}                  // 确保 IMC 已关联(幂等)
    virtual void imePrepare(JZWindow* w, int fontPixelSize) {}  // 组字窗/候选窗字体
    virtual void imeCancel(JZWindow* w) {}                      // 放弃未上屏的组字
    virtual void imeUpdateUi(JZWindow* w, bool show) {}          // 重发 WM_IME_SETCONTEXT
    // 组字窗/候选窗的位置 (屏幕逻辑坐标, 由焦点控件算出, 见 JZWidget::imeWindowRect)。
    // 真窗口不需要它: 文本控件自己用 IMM 接口设ImmSetCompositionWindow/CandidateWindow。
    // 虚拟桌面用它: 那边的文本控件没有 HWND, 只能把位置交给后端转给**真正拿着 IMC 的
    // 宿主窗口** —— 否则候选框停在 IME 的默认位置, 不跟虚拟桌面里的输入行走。
    virtual void imeSetPosition(JZWindow* w, const JZRect& lineRect, const JZPoint& candPos) {
        (void)w; (void)lineRect; (void)candPos;
    }

    // ---- 拖放 (OLE IDropTarget; 只有真窗口能接收外部拖放) ----
    // 窗口创建后登记成系统拖放目标, 销毁前必须注销。虚拟桌面没有原生窗口,
    // 默认实现为空 (拖放只对真窗口有意义)。
    virtual void registerDragAndDrop(JZWindow* w) { (void)w; }
    virtual void revokeDragAndDrop(JZWindow* w) { (void)w; }
};

// 当前平台的默认窗口后端 (JZWindow 构造时装它)。所有权归调用方。
JZFRAMEWORK_API JZWindowBackend* jzCreateDefaultWindowBackend();

#endif // JZ_WINDOW_BACKEND_H_
