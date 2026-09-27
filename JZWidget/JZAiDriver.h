#ifndef JZ_AI_DRIVER_H_
#define JZ_AI_DRIVER_H_
#include "JZWidget.h"
#include "JZTimer.h"
#include "JZSignal.h"
#include <functional>

class JZPushButton;

// ==================== AI 鼠标覆盖层 ====================
// 全窗蒙版: 半透明遮罩 + 命中透传 (setPassThrough) 让 AI 合成点击可穿到目标,
// 由 JZWindow::setInputGate 配套屏蔽真实用户输入 (见 JZWindow::dispatchMouse)。
// 绘制 AI 光标箭头/点击涟漪/左下角状态条; 唯一可命中的子控件是"取消"按钮。
// 注意: 覆盖层 setOpaque(0) —— 不向身后的内容贡献"不透明覆盖", 否则重绘剔除
// 会冻结蒙版下的内容, 移动中的 AI 光标在帧缓冲里留下旧帧残影。
// 由 JZAiDriver 驱动; 坐标为根窗口逻辑像素, 覆盖层位于根 (0,0) 处两者一致。
class JZFRAMEWORK_API JZAiOverlay : public JZWidget
{
    JZ_OBJECT_DECLARE(JZAiOverlay)

public:
    explicit JZAiOverlay(JZWidget* parent = nullptr);

    void setCursorPos(const JZPoint& pos);      // AI 光标位置
    void setStatusText(const std::string& text);// 状态条文本 (空 = 隐藏状态条)
    void startClickEffect(const JZPoint& pos);  // 点击涟漪
    void tickEffect();                          // 涟漪推进 (由驱动每 tick 调用)
    void syncToParent();                        // 尺寸对齐父窗口 (含新旧区域失效)

jz_signals:
    JZSigVoid sigCancelRequested;               // 用户点击"取消"或按 Esc

protected:
    void paintEvent(JZPainterEvent* event) override;
    void keyPressEvent(JZKeyEvent* event) override;   // 蒙版期间 Esc=取消, 其余吞掉
    void drawArrow(JZPainter& p, const JZPoint& pos) const;   // 扫描线填充箭头
    void layoutBanner();                        // 状态条布局 (确定性的, 布局与绘制分离)

private:
    JZPushButton* m_cancelBtn = nullptr;
    JZPoint m_cursorPos;
    std::string m_statusText;
    JZRect m_bannerRect;                        // 状态条矩形 (空 = 隐藏)
    bool m_effectOn = false;
    JZPoint m_effectPos;
    int m_effectTick = 0;
};

// ==================== AI 驱动 (ai-native-design L2 可视化子集) ====================
// 操作序列: 状态条显示操作 -> AI 光标平滑移动 (缓动) 到目标中心 ->
//           (click 类) 点击涟漪 + 合成真实鼠标点击 -> (其它) 直调动作 -> 结束回报。
// 异步: 通过 sigDone(seq, ok, message) 回报; 状态条上的"取消"按钮可中断序列。
class JZFRAMEWORK_API JZAiDriver : public JZObject
{
    JZ_OBJECT_DECLARE(JZAiDriver)

public:
    explicit JZAiDriver(JZWindow* root, JZObject* parent = nullptr);

    // 移动到控件中心; performClick=true 时到达后合成真实点击 (带涟漪)。
    // onArrive: 到达后执行的动作 (setValue/setText/select 等直调)。
    // statusText 显示在左下角状态条。返回序列号 (匹配 sigDone); 忙时返回 -1。
    int moveToWidget(JZWidget* target, bool performClick, const std::string& statusText,
                     std::function<void()> onArrive = nullptr);

    void cancel();                    // 用户中断当前序列 (下个 tick 生效)

    // 切换驱动作用的根窗口 (UI MCP 的 ui_activate_window 换目标窗口时调用):
    // 覆盖层跟着迁到新窗口; 进行中的序列作废(回报取消)。传 nullptr 只解绑。
    // 覆盖层挂在根窗口上(受它拥有), 故切换后旧窗口不再有蒙版。
    void setRoot(JZWindow* root);
    // 全屏蒙版是否在岗 (真实用户输入被丢弃; 仅取消按钮/Esc 可操作)
    bool maskActive() const { return m_phase != PhaseIdle; }
    bool busy() const { return m_phase != PhaseIdle; }

jz_signals:
    JZSignal<int, bool, const std::string&> sigDone;   // (seq, ok, message)

protected:
    void onTick();
    void doArrive();   // 到达后执行动作 (经 runOnObject 投递, 脱离 timer 栈:
                       // 合成点击/onArrive 可能开模态循环, 嵌在 processTimers
                       // 栈内会被重入的 processTimers 再次触发 onTick)
    void finish(bool ok, const std::string& msg);

private:
    enum Phase { PhaseIdle, PhaseMoving, PhaseActing };

    JZWindow* m_root = nullptr;
    JZAiOverlay* m_overlay = nullptr;
    JZTimer* m_timer = nullptr;

    Phase m_phase = PhaseIdle;
    int m_seq = 0;
    JZPoint m_cursorPos { 30, 30 };   // AI 光标常驻位置 (初始左上角)
    JZPoint m_from;
    JZPoint m_to;
    int m_tick = 0;
    int m_totalTicks = 0;
    int m_actTicks = 0;
    bool m_performClick = false;
    bool m_cancelRequested = false;
    std::function<void()> m_onArrive;

    static const int kTickMs = 30;
    static const int kActTicks = 14;  // 点击效果停留 ~420ms
};

#endif // JZ_AI_DRIVER_H_
