#ifndef JZ_CODE_AGENT_CHATVIEW_H_
#define JZ_CODE_AGENT_CHATVIEW_H_

#include "JZWidget.h"
#include "JZFont.h"

#include <string>
#include <vector>

// 独立的对话显示控件: 不基于 JZTextEdit, 全部用 JZPainter 自绘。
// 负责把消息渲染成"气泡卡片 + 自动换行 + 垂直滚动", 自行管理文本测量与布局。
class ChatView : public JZWidget
{
    JZ_OBJECT_DECLARE(ChatView)

public:
    enum Role { User, Assistant, Tool, ToolResult, Error, Info };

    explicit ChatView(JZWidget* parent = nullptr);

    // 各类消息追加接口 (流式 delta 会续接到同一助手气泡)
    void appendUser(const std::string& text);
    void appendAssistantDelta(const std::string& text);
    void appendTool(const std::string& name, const std::string& text);
    void appendToolResult(const std::string& text);
    void appendError(const std::string& text);
    void appendInfo(const std::string& text);

    // 结束当前流式助手气泡 (agent 结束一轮时调用)
    void endStream();
    void clear();

    // 立即滚到底 (清空/初始化时用)
    void scrollToBottom();

protected:
    void paintEvent(JZPainterEvent* event) override;
    void resizeEvent(JZResizeEvent* event) override;
    void wheelEvent(JZMouseEvent* event) override;

private:
    struct Msg {
        Role role;
        std::string name;                  // tool 名称
        std::string text;
        std::vector<std::string> lines;    // 缓存的换行结果
        int cardH = 0;
    };

    void layoutMsg(Msg& m);
    void relayout();
    void afterChange();                    // 内容变化后更新滚动并触发重绘
    void updateScroll(bool follow);
    int  contentHeight() const;

    static void wrapText(const std::string& text, const JZFont& font,
                         int maxW, std::vector<std::string>& out);

    std::vector<Msg> m_msgs;
    Msg* m_streamMsg = nullptr;            // 当前正在流式追加的助手消息
    int  m_scrollY = 0;
    bool m_autoFollow = true;              // 是否自动跟随到底部
    int  m_totalH = 0;                     // 内容总高 (含上下外边距)
};

#endif // !JZ_CODE_AGENT_CHATVIEW_H_
