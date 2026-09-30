#ifndef JZ_WINDOW_BACKEND_H_
#define JZ_WINDOW_BACKEND_H_
#include "JZWidget.h"
#include "JZImage.h"

#include <string>

class JZWindow;

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
    // 虚拟桌面恒为 1 (它是纯逻辑桌面, 与宿主显示器无关)。
    virtual float deviceScale(JZWindow* w) const = 0;
    // 绘制谁来驱动: false(默认) = 平台的消息循环拉脏区 (真窗口: WM_PAINT / 主线程的
    // paint task); true = **窗口自己的线程就地画** (虚拟窗口: 没有平台消息, 帧缓冲由
    // 合成方按需取)。见 JZWindow::event(JZEvent_invalidate) —— 这一条决定了"谁碰这块
    // 帧缓冲": 虚拟窗口的绘制永远发生在它自己所属的线程上, 于是多线程(每窗口一个线程)
    // 时不会有"主线程替别的线程画窗口"的竞态。
    virtual bool paintsOnDemand() const { return false; }

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
};

// 当前平台的默认窗口后端 (JZWindow 构造时装它)。所有权归调用方。
JZFRAMEWORK_API JZWindowBackend* jzCreateDefaultWindowBackend();

#endif // JZ_WINDOW_BACKEND_H_
