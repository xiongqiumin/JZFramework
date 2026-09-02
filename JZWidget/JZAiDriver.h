#ifndef JZ_AI_DRIVER_H_
#define JZ_AI_DRIVER_H_

#include "JZWidget.h"
#include "JZTimer.h"
#include "JZSignal.h"
#include <functional>

class JZPushButton;

// ==================== AI 鼠标覆盖层 ====================
// 全窗覆盖 + 命中透传 (setPassThrough): 绘制 AI 光标箭头/点击涟漪/左下角状态条,
// 唯一可命中的子控件是"取消"按钮 (透传层的子控件正常参与命中)。
// 由 JZAiDriver 驱动; 坐标为根窗口逻辑像素, 覆盖层位于根 (0,0) 处两者一致。
class JZAiOverlay : public JZWidget
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
    JZSigVoid sigCancelRequested;               // 用户点击"取消"

protected:
    void paintEvent(JZPainterEvent* event) override;
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
class JZAiDriver : public JZObject
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
    bool busy() const { return m_phase != PhaseIdle; }

jz_signals:
    JZSignal<int, bool, const std::string&> sigDone;   // (seq, ok, message)

protected:
    void onTick();
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
