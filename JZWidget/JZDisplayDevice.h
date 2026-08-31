#ifndef JZ_DISPLAY_DEVICE_H_
#define JZ_DISPLAY_DEVICE_H_

#include "JZWidget.h"
#include "JZLayer.h"
#include "JZRect.h"
#include "JZColor.h"
#include "JZMouseEvent.h"

#include <string>
#include <vector>

class JZImGuiBackend;

// 显示设备: 顶层窗口 = 原生窗口 + 渲染后端 + 事件分发。
// 职责:
//   - 创建/销毁原生 Win32 窗口, 泵送其消息 (WndProc 在本类实现);
//     鼠标/键盘消息经 逻辑像素 换算后命中检测并分发到控件树;
//   - refresh() 里遍历 JZWidget 树 (含剔除), 收集 JZDrawTask 到 JZLayer;
//   - 交给 JZImGuiBackend 回放到 ImDrawList 并 Present (ImGui 只负责绘图)。
//
// 坐标系: 控件树/绘制任务为逻辑像素; m_scale (DPI 缩放) 负责逻辑 <-> 物理换算。
// 头文件不引入任何 Win32 类型 (HWND 以 void* 持有, WndProc 在 .cpp 实现)。
class JZDisplayDevice : public JZWidget
{
    JZ_OBJECT_DECLARE(JZDisplayDevice)

public:
    static void refresh();        // 重绘所有有脏区的设备
    static void processEvents();  // 泵送所有设备的窗口消息 (每帧先于 refresh 调用)

    JZDisplayDevice(int w, int h, const std::string& title = "JZWidget");
    ~JZDisplayDevice();

    JZLayer* layer();

    void setTitle(const std::string& title);
    std::string title() const;

    // 显示/隐藏原生窗口 (对话框等在构造后隐藏, 按需显示)。
    void setWindowVisible(bool visible);

    // 模态支持: 屏蔽/恢复除 exclude 外所有设备窗口的输入。
    static void setOthersInputEnabled(JZDisplayDevice* exclude, bool enabled);

    void makeInvalidate(const JZRect& rc);   // 标记脏区 (JZWidget::invalidate 使用)

    // 尺寸变化同步到原生窗口
    virtual void resize(int w, int h) override;

    // ---- WndProc / 渲染后端使用的接口 ----
    float scale() const;                  // DPI 缩放 (逻辑 -> 物理像素)
    JZImGuiBackend* backend();            // 渲染后端 (可为空)
    void updateDpiScale();                // 窗口 DPI 变化后重建字体
    void dispatchMouseEvent(JZMouseEvent::MouseType type, JZMouseEvent::Button button,
                            const JZPoint& globalPos, int wheelDelta = 0);
    void dispatchKeyEvent(uint32_t key, bool pressed);
    void onWindowResized(int logicalW, int logicalH);
    virtual void requestClose();   // 原生窗口 WM_CLOSE (默认退出应用; 对话框重写为 reject)

private:
    virtual void paintEvent(JZPainterEvent* event) override;

    void pumpMessages();   // 泵送本窗口消息
    void redraw();
    JZWidget* hitTest(const JZPoint& globalPos);
    static JZWidget* hitTest(JZWidget* w, const JZPoint& globalPos);
    void collectFrontToBack(JZWidget* w, const JZRect& area, std::vector<JZRect>& coverage, std::vector<JZWidget*>& out);

    static std::vector<JZDisplayDevice*> displayList;

    JZImGuiBackend* m_backend = nullptr;
    JZLayer m_layer;
    JZRect m_invalidRect;
    JZWidget* m_mouseGrab = nullptr;   // 按下后锁定目标 (move/release 跟随)
    std::string m_title;
    void* m_hwnd = nullptr;            // HWND (避免头文件引入 Win32 类型)
    float m_scale = 1.0f;              // DPI 缩放: 物理像素 = 逻辑像素 * m_scale
};

#endif
