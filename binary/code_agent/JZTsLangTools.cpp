#include "JZTsLangTools.h"

#include "json/json.h"

using namespace std;

JZTsTool::JZTsTool(JZTsLangServer *server, TsMethod method)
    : m_server(server), m_method(method)
{
    ToolDefine def;

    // 公共参数
    ParamSchema file = makeParam("file", STRING, "TypeScript/JavaScript 文件绝对路径");
    ParamSchema line = makeParam("line", NUMBER, "行号，从 1 开始");
    ParamSchema offset = makeParam("offset", NUMBER, "列偏移，从 1 开始（字符位置）");

    switch (method)
    {
        case TsMethod::Definition:
            def.name = "ts_definition";
            def.description = "查找光标处符号的定义位置（文件 + 行列）。基于 TypeScript 7 (TSGO) API。";
            def.parameters = {file, line, offset};
            def.required_parameters = {"file", "line", "offset"};
            break;
        case TsMethod::References:
            def.name = "ts_references";
            def.description = "查找符号在项目内的所有引用位置。可传 name（类名 / 函数名）"
                             "按名字定位，免去精确行列；不传 name 时必须传 line/offset。";
            {
                ParamSchema name = makeParam("name", STRING, "符号名（类名 / 函数名），按名字定位，二选一");
                def.parameters = {file, name, line, offset};
            }
            def.required_parameters = {"file"}; // name 与 line/offset 二选一
            break;
        case TsMethod::QuickInfo:
            def.name = "ts_quickinfo";
            def.description = "获取光标处符号的类型签名与文档（等价于编辑器 hover）。";
            def.parameters = {file, line, offset};
            def.required_parameters = {"file", "line", "offset"};
            break;
        case TsMethod::Rename:
            def.name = "ts_rename";
            def.description = "查找光标处符号可被重命名的所有位置（定义 + 引用）。";
            def.parameters = {file, line, offset};
            def.required_parameters = {"file", "line", "offset"};
            break;
        case TsMethod::Completion:
            def.name = "ts_completion";
            def.description = "获取光标处的代码补全项。";
            def.parameters = {file, line, offset};
            def.required_parameters = {"file", "line", "offset"};
            break;
        case TsMethod::DocumentHighlights:
            def.name = "ts_document_highlights";
            def.description = "高亮光标处符号在当前文件内的所有出现位置。";
            def.parameters = {file, line, offset};
            def.required_parameters = {"file", "line", "offset"};
            break;
        case TsMethod::Diagnostics:
            def.name = "ts_diagnostics";
            def.description = "返回文件的语法 / 语义 / 建议诊断（错误与警告）。";
            def.parameters = {makeParam("file", STRING, "文件绝对路径")};
            def.required_parameters = {"file"};
            break;
        case TsMethod::Source:
        {
            def.name = "ts_source";
            def.description = "返回光标处符号（函数 / 类 / 接口 / 变量等）的声明源码正文。"
                             "import/export 别名自动解析到真正的定义文件。改代码前用它看函数体，"
                             "不用再拿行列去翻文件。";
            ParamSchema maxc = makeParam("max_chars", NUMBER, "正文截断上限（默认 8000 字符）");
            def.parameters = {file, line, offset, maxc};
            def.required_parameters = {"file", "line", "offset"};
            break;
        }
        case TsMethod::Outline:
            def.name = "ts_outline";
            def.description = "列出文件顶层声明（函数 / 类 / 接口 / 类型别名 / 变量 / 命名空间）"
                             "及其成员的行号范围，类 / 接口带内部成员列表。先看结构再决定读哪段。";
            def.parameters = {makeParam("file", STRING, "TypeScript/JavaScript 文件绝对路径")};
            def.required_parameters = {"file"};
            break;
        case TsMethod::Members:
        {
            def.name = "ts_members";
            def.description = "列出符号的成员 / 签名：类/接口返回属性与方法签名；"
                             "函数返回调用签名；模块返回导出清单。可传 name（类名 / 函数名）"
                             "按名字定位，免去精确行列；不传 name 时必须传 line/offset。";
            ParamSchema maxm = makeParam("max_members", NUMBER, "成员数量上限（默认 100）");
            ParamSchema name = makeParam("name", STRING, "符号名（类名 / 函数名），按名字定位，二选一");
            def.parameters = {file, name, line, offset, maxm};
            def.required_parameters = {"file"}; // name 与 line/offset 二选一
            break;
        }
    }

    setToolDefine(def);
}

void JZTsTool::call(const ToolCall &params)
{
    const Json::Value &p = params.params;
    string file = p.get("file", "").asString();
    int line = p.get("line", 0).asInt();
    int offset = p.get("offset", 0).asInt();

    if (file.empty())
    {
        finish(params, false, "参数 file 不能为空");
        return;
    }

    Json::Value req;
    req["file"] = file;
    if (line > 0)
        req["line"] = line;
    if (offset > 0)
        req["offset"] = offset;

    if (m_method == TsMethod::Source && p.isMember("max_chars"))
        req["max_chars"] = p["max_chars"].asInt();
    if (m_method == TsMethod::Members && p.isMember("max_members"))
        req["max_members"] = p["max_members"].asInt();
    if (p.isMember("name"))
        req["name"] = p["name"].asString();

    m_server->request(m_method, req, [this, params](bool ok, const string &payload) {
        this->finish(params, ok, payload);
    });
}
