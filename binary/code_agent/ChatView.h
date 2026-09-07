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
    enum Role { User, Assistant, Thinking, Tool, ToolResult, Error, Info };

    explicit ChatView(JZWidget* parent = nullptr);

    // 各类消息追加接口 (流式 delta 会续接到同一气泡)
    void appendUser(const std::string& text);
    // 整条助手消息: 等价于 appendAssistantDelta + endStream,
    // 用于一次性重建(如导入工作区历史), 保证每条各占一个气泡。
    void appendAssistant(const std::string& text);
    void appendAssistantDelta(const std::string& text);
    // 思考过程: 实时流式增量会续接同一张思考卡片; 思考结束后由
    // 下一条非思考消息(正文/工具调用/结束)自动折叠, 点击可展开。
    void appendThinking(const std::string& text);
    void appendThinkingDelta(const std::string& text);
    // 工具调用与它的输出合并显示在同一张卡片里:
    // appendToolResult 会把结果回填给最近一条还没配对的工具调用卡;
    // 没有待配对调用时才退化成独立的"工具结果"卡片(历史数据兼容)。
    void appendTool(const std::string& name, const std::string& tool_id, const std::string& text);
    void appendToolResult(const std::string& tool_id, const std::string& text);
    void appendError(const std::string& text);
    void appendInfo(const std::string& text);

    // 结束当前流式助手气泡 (agent 结束一轮时调用); 若思考尚未结束会一并折叠
    void endStream();
    void clear();

    // 显示/隐藏末尾"等待回复中..."状态卡(有指令在排队等待时由上层调用)
    void setWait(bool show);

    // 立即滚到底 (清空/初始化时用)
    void scrollToBottom();

protected:
    void paintEvent(JZPainterEvent* event) override;
    void resizeEvent(JZResizeEvent* event) override;
    void wheelEvent(JZMouseEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;

private:
    struct Msg {
        Role role = User;
        std::string name;                  // tool 名称
        std::string id;
        std::string text;                  // 正文 / 工具调用参数 / 思考文本
        std::vector<std::string> lines;    // 缓存的换行结果
        std::string resultText;            // 工具结果(回填到工具调用卡时用)
        std::vector<std::string> resultLines; // 结果换行结果
        bool hasResult = false;            // 工具卡已收到输出
        bool collapsible = false;          // 思考卡: 结束后折叠、可点击展开
        bool collapsed = false;            // 是否处于折叠状态
        int cardH = 0;
    };

    void layoutMsg(Msg& m);
    void relayout();
    void afterChange();                    // 内容变化后更新滚动并触发重绘
    void updateScroll(bool follow);
    int  contentHeight() const;

    // 结束当前打开的流式卡: 若它是思考卡则自动折叠(思考完毕)。
    // 返回 true 表示确实折叠了一个打开的思考卡。
    bool closeStream();
    // 新开一张卡作为当前流式目标, 返回其索引 (先关掉旧的流式卡)。
    int  openStream(Role role);
    int  hitTestMsg(int contentY) const;   // 内容坐标 -> 消息索引 (-1 无)

    static void wrapText(const std::string& text, const JZFont& font,
                         int maxW, std::vector<std::string>& out);

    std::vector<Msg> m_msgs;
    int  m_streamIdx = -1;                 // 当前正在流式追加的卡(思考/助手), -1 无
    std::map<std::string,int> m_pendingToolIdx;     // 已发出、等待输出回填的工具卡(FIFO)
    int  m_scrollY = 0;
    bool m_autoFollow = true;              // 是否自动跟随到底部
    int  m_totalH = 0;                     // 内容总高 (含上下外边距)

    bool m_waitShow = false;               // 是否显示"等待回复中..."状态卡
    std::vector<std::string> m_waitLines;  // 等待卡换行结果
    int  m_waitH = 0;                      // 等待卡高度 (含上方间距)
};

#endif // !JZ_CODE_AGENT_CHATVIEW_H_
