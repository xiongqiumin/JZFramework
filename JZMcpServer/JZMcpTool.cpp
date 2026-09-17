#include "JZMcpTool.h"

JZ_OBJECT_IMPL(JZMcpTool, JZObject)

JZMcpTool::JZMcpTool(JZObject* parent)
    : JZObject(parent)
    , sigFinished(this)
    , sigCanceled(this)
    , sigNotify(this)
{
    // 默认 inputSchema 为合法的空 object
    m_inputSchema = Json::Value(Json::objectValue);
    m_inputSchema["type"] = "object";
    m_inputSchema["properties"] = Json::Value(Json::objectValue);
}

JZMcpTool::~JZMcpTool()
{
}

void JZMcpTool::run(const Json::Value& id, const Json::Value& args)
{
    (void)id;
    (void)args;
    // 默认实现: 报告未实现(作为错误结果通过 sigFinished 回送)
    Json::Value result(Json::objectValue);
    Json::Value content(Json::arrayValue);
    Json::Value item(Json::objectValue);
    item["type"] = "text";
    item["text"] = "JZMcpTool::run() not implemented";
    content.append(item);
    result["content"] = content;
    result["isError"] = true;
    sigFinished.emit(id, result);
}

void JZMcpTool::cancel(const Json::Value& id)
{
    // 基类默认: 仅发射取消信号。子类应重写本函数维护自身取消状态。
    sigCanceled.emit(id);
}
