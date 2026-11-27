// JZAgentOmni 单测(纯文本转换: 不需要模型 / 网络 / 事件循环)
//
// 覆盖:
//   请求方向 —— system/tools 内联、增量(fromIndex)、图片占位、
//               assistant(tool_calls) 与 tool 结果的回放、思考开关
//   响应方向 —— 特殊 token 过滤、<think> 抽取、<tool_call> / <function=...> 解析、
//               分片切断标签、输出截断降级、字面量 '<' 不误伤
// 期望输出: ALL PASS

#include <stdio.h>

#include <string>

#include "JZAgentOmni.h"

using namespace std;

static int g_fail = 0;

static void check(bool ok, const string &what)
{
    printf("%s %s\n", ok ? "[ok]  " : "[fail]", what.c_str());
    if (!ok)
        g_fail++;
}

static bool has(const string &text, const string &part)
{
    return text.find(part) != string::npos;
}

static Json::Value parseJson(const char *text)
{
    Json::Value root;
    Json::Reader reader;
    reader.parse(text, root);
    return root;
}



int test_omni(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    JZAgentOmni omni;

    // ------------------------------------------------------------------
    // 1) 请求方向: system + tools + user
    // ------------------------------------------------------------------
    printf("== 请求方向 ==\n");
    {
        Json::Value request = parseJson(
            "{\"messages\":["
            "{\"role\":\"system\",\"content\":\"SYS\"},"
            "{\"role\":\"user\",\"content\":\"HI\"}],"
            "\"tools\":[{\"type\":\"function\",\"function\":"
            "{\"name\":\"f\",\"description\":\"d\",\"parameters\":{\"type\":\"object\"}}}],"
            "\"chat_template_kwargs\":{\"enable_thinking\":false}}");

        string prompt = omni.buildPrompt(request);
        check(has(prompt, "<|im_start|>system\nSYS"), "system 段");
        check(has(prompt, "# Tools"), "tools 标题");
        check(has(prompt, "\"f\""), "tools 定义内联");
        check(has(prompt, "<|im_start|>user\nHI<|im_end|>"), "user 段");
        check(has(prompt, "<|im_start|>assistant\n<think>\n</think>"), "assistant 头(思考预闭合)");
    }

    // ------------------------------------------------------------------
    // 2) 请求方向: fromIndex 只拼新增轮次
    // ------------------------------------------------------------------
    {
        Json::Value request = parseJson(
            "{\"messages\":["
            "{\"role\":\"system\",\"content\":\"SYS\"},"
            "{\"role\":\"user\",\"content\":\"FIRST\"},"
            "{\"role\":\"user\",\"content\":\"SECOND\"}]}");

        string prompt = omni.buildPrompt(request, 2);
        check(!has(prompt, "SYS"), "fromIndex: 不重复 system");
        check(!has(prompt, "FIRST"), "fromIndex: 不重复旧消息");
        check(has(prompt, "SECOND"), "fromIndex: 只带新消息");
        check(has(prompt, "<|im_start|>assistant"), "fromIndex: 仍补 assistant 头");
    }

    // ------------------------------------------------------------------
    // 3) 请求方向: 多模态 content 数组 -> 占位标记
    // ------------------------------------------------------------------
    {
        Json::Value request = parseJson(
            "{\"messages\":[{\"role\":\"user\",\"content\":["
            "{\"type\":\"text\",\"text\":\"看图\"},"
            "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,xx\"}}]}]}");

        string prompt = omni.buildPrompt(request);
        check(has(prompt, "看图"), "content 数组: 文本");
        check(has(prompt, "<|vision_start|><|image_pad|><|vision_end|>"), "content 数组: 图片占位");
    }

    // ------------------------------------------------------------------
    // 4) 请求方向: assistant(tool_calls) + tool 结果回放
    // ------------------------------------------------------------------
    {
        Json::Value request = parseJson(
            "{\"messages\":["
            "{\"role\":\"user\",\"content\":\"查天气\"},"
            "{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[{\"id\":\"call_0\",\"type\":\"function\","
            "\"function\":{\"name\":\"weather\",\"arguments\":\"{\\\"city\\\":\\\"北京\\\"}\"}}]},"
            "{\"role\":\"tool\",\"tool_call_id\":\"call_0\",\"content\":\"晴\"}]}");

        string prompt = omni.buildPrompt(request);
        check(has(prompt, "<tool_call>{"), "历史: 工具调用标记");
        check(has(prompt, "\"weather\""), "历史: 工具名");
        check(has(prompt, "北京"), "历史: 参数");
        check(has(prompt, "<tool_response>\n晴\n</tool_response>"), "历史: 工具结果");
    }

    // ------------------------------------------------------------------
    // 5) 请求方向: 打开思考时不预闭合
    // ------------------------------------------------------------------
    {
        Json::Value request = parseJson(
            "{\"messages\":[{\"role\":\"user\",\"content\":\"HI\"}],"
            "\"chat_template_kwargs\":{\"enable_thinking\":true}}");
        string prompt = omni.buildPrompt(request);
        size_t tail = prompt.rfind("<|im_start|>assistant");
        check(tail != string::npos && !has(prompt.substr(tail), "</think>"),
              "思考开启: 不预闭合");
    }

    // ------------------------------------------------------------------
    // 6) 响应方向: 特殊 token + 普通文本
    // ------------------------------------------------------------------
    printf("\n== 响应方向 ==\n");
    {
        Json::Value response = omni.parseResponse("hello<|im_end|><|audio_pad|> world", "local");
        const Json::Value &choice = response["choices"][0];
        check(choice["message"]["content"].asString() == "hello world", "过滤特殊 token");
        check(choice["finish_reason"].asString() == "stop", "finish_reason=stop");
        check(!choice["message"].isMember("tool_calls"), "无工具调用");
        check(response["usage"]["total_tokens"].asInt() == 0, "usage 默认 0");
    }

    // ------------------------------------------------------------------
    // 7) 响应方向: <think> 抽成 reasoning_content
    // ------------------------------------------------------------------
    {
        Json::Value response = omni.parseResponse("<think>\n想一下\n</think>\n答案", "local");
        const Json::Value &message = response["choices"][0]["message"];
        check(message["content"].asString() == "答案", "思考: 正文去掉思考");
        check(message["reasoning_content"].asString() == "想一下\n", "思考: 内容进 reasoning_content");
    }

    // ------------------------------------------------------------------
    // 8) 响应方向: <tool_call> JSON
    // ------------------------------------------------------------------
    {
        Json::Value response = omni.parseResponse(
            "<tool_call>{\"name\":\"weather\",\"arguments\":{\"city\":\"北京\"}}</tool_call>", "local");
        const Json::Value &choice = response["choices"][0];
        const Json::Value &call = choice["message"]["tool_calls"][0];
        check(call["function"]["name"].asString() == "weather", "tool_call: 名字");
        check(has(call["function"]["arguments"].asString(), "北京"), "tool_call: 参数(JSON 字符串)");
        check(call["id"].asString() == "call_0", "tool_call: id");
        check(choice["finish_reason"].asString() == "tool_calls", "tool_call: finish_reason");
    }

    // ------------------------------------------------------------------
    // 9) 响应方向: <function=...> 形式
    // ------------------------------------------------------------------
    {
        Json::Value response = omni.parseResponse(
            "先执行\n<function=run_cmd><parameter=cmd>ls -l</parameter></function>", "local");
        const Json::Value &message = response["choices"][0]["message"];
        const Json::Value &call = message["tool_calls"][0];
        check(call["function"]["name"].asString() == "run_cmd", "function: 名字");
        check(has(call["function"]["arguments"].asString(), "ls -l"), "function: 参数");
        check(message["content"].asString() == "先执行\n", "function: 正文保留");
    }

    // ------------------------------------------------------------------
    // 10) 响应方向: 多个工具调用
    // ------------------------------------------------------------------
    {
        Json::Value response = omni.parseResponse(
            "<tool_call>{\"name\":\"a\",\"arguments\":{}}</tool_call>"
            "<tool_call>{\"name\":\"b\",\"arguments\":{}}</tool_call>");
        const Json::Value &calls = response["choices"][0]["message"]["tool_calls"];
        check(calls.size() == 2, "多调用: 数量");
        check(calls[1]["id"].asString() == "call_1", "多调用: id 递增");
    }

    // ------------------------------------------------------------------
    // 11) 流式: 标签被分片切断
    // ------------------------------------------------------------------
    printf("\n== 流式 ==\n");
    {
        omni.beginStream();
        const char *pieces[] = { "he", "llo<thi", "nk>思考", "</think>答", "案" };
        string content;
        string thinking;
        for (const char *piece : pieces)
        {
            Json::Value chunk = omni.parseDelta(piece);
            const Json::Value &delta = chunk["choices"][0]["delta"];
            if (delta.isMember("content"))
                content += delta["content"].asString();
            if (delta.isMember("reasoning_content"))
                thinking += delta["reasoning_content"].asString();
        }
        Json::Value last = omni.endStream();
        const Json::Value &delta = last["choices"][0]["delta"];
        if (delta.isMember("content"))
            content += delta["content"].asString();

        check(content == "hello答案", "流式: 正文拼接正确 (" + content + ")");
        check(thinking == "思考", "流式: 思考拼接正确 (" + thinking + ")");
        check(!has(content, "<") && !has(content, "thi"), "流式: 半截标签不漏出");
        check(last["choices"][0]["finish_reason"].asString() == "stop", "流式: finish_reason");
    }

    // ------------------------------------------------------------------
    // 12) 流式: 工具调用跨分片, 在 endStream 整批给出
    // ------------------------------------------------------------------
    {
        omni.beginStream();
        omni.parseDelta("<tool_call>{\"name\":\"x\",");
        omni.parseDelta("\"arguments\":{}}</tool_call>");
        Json::Value last = omni.endStream();
        const Json::Value &delta = last["choices"][0]["delta"];
        check(delta.isMember("tool_calls"), "流式工具: endStream 给出");
        check(delta["tool_calls"][0]["function"]["name"].asString() == "x", "流式工具: 名字");
        check(last["choices"][0]["finish_reason"].asString() == "tool_calls", "流式工具: finish_reason");
    }

    // ------------------------------------------------------------------
    // 13) 输出截断: 只有开标签 -> 降级成正文
    // ------------------------------------------------------------------
    {
        Json::Value response = omni.parseResponse("<tool_call>{\"name\":\"x", "local");
        const Json::Value &message = response["choices"][0]["message"];
        check(!message.isMember("tool_calls"), "截断: 不产出工具调用");
        check(has(message["content"].asString(), "<tool_call>"), "截断: 原样留在正文");
    }

    // ------------------------------------------------------------------
    // 14) 字面量 '<' 不误伤
    // ------------------------------------------------------------------
    {
        Json::Value response = omni.parseResponse("如果 a < b 且 c > d 就成立", "local");
        check(response["choices"][0]["message"]["content"].asString() == "如果 a < b 且 c > d 就成立",
              "字面量 '<' 保留");
    }

    printf("\n%s\n", g_fail == 0 ? "ALL PASS" : "FAILED");
    return g_fail == 0 ? 0 : 1;
}
