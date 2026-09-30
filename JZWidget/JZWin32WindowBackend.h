#ifndef JZ_WIN32_WINDOW_BACKEND_H_
#define JZ_WIN32_WINDOW_BACKEND_H_
#include "JZWindowBackend.h"

// ==================== Windows 真窗口后端 ====================
// 平台相关的东西全在这里: RegisterClassExW/CreateWindowExW、窗口过程(WndProc)、
// 鼠标抓取(SetCapture)、光标(SetCursor)、DPI、IMM(输入法)、JZWin32Thread 消息循环。
//
// 头文件**零 Win32 类型**: 句柄与 IMC 都以 void* 存, 转换在 .cpp 里做 —— 于是
// "JZWidget 的平台代码"可以一眼看出边界(本文件 + JZVirtualWindowBackend)。
class JZFRAMEWORK_API JZWin32WindowBackend : public JZWindowBackend
{
public:
    JZWin32WindowBackend();
    ~JZWin32WindowBackend() override;

    bool create(JZWindow* w) override;
    void destroy(JZWindow* w) override;
    void onPlatformDestroyed(JZWindow* w) override;
    bool isCreated() const override;
    void* handle() const override;

    void show(JZWindow* w) override;
    void hide(JZWindow* w) override;
    void setTitle(JZWindow* w, const std::string& title) override;
    void setClientSize(JZWindow* w, int cw, int ch) override;
    void setScreenPos(JZWindow* w, int x, int y) override;

    JZPoint clientToScreen(JZWindow* w, const JZPoint& pt) const override;
    JZPoint screenToClient(JZWindow* w, const JZPoint& pt) const override;
    float deviceScale(JZWindow* w) const override;
    bool containsScreenPoint(JZWindow* w, int x, int y) const override;

    void setTop(JZWindow* w, bool on) override;
    void showMinimized(JZWindow* w) override;
    void showMaximized(JZWindow* w) override;
    void showNormal(JZWindow* w) override;
    void activate(JZWindow* w) override;
    void updateCursor(JZWindow* w) override;
    void setCapture(JZWindow* w, bool on) override;

    int    execModal(JZWindow* w, bool disableOwner) override;
    JZImage grabClient(JZWindow* w) override;

    void imeAssociate(JZWindow* w) override;
    void imePrepare(JZWindow* w, int fontPixelSize) override;
    void imeCancel(JZWindow* w) override;
    void imeUpdateUi(JZWindow* w, bool show) override;

    // 缓存的默认输入上下文 (IMC): 窗口创建后**始终保持关联**, 不随焦点断开关联 ——
    // 断开会让输入法把中/英状态重置成默认中文(见 updateImeContext 的说明)。
    void* imeContext() const { return m_imeContext; }

private:
    void* m_hwnd = nullptr;        // HWND (本头文件不引 windows.h)
    void* m_imeContext = nullptr;  // HIMC
};

#endif // JZ_WIN32_WINDOW_BACKEND_H_
