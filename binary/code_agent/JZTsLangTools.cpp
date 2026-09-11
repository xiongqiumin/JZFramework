#include "JZTsLangTools.h"

#include "json/json.h"

using namespace std;

JZTsTool::JZTsTool(JZTsLangServer *server, TsMethod method)
    : m_server(server), m_method(method)
{
    ToolDefine def;

    // 公共参数：所有查询只收「文件 + 符号名」，不接受行列 —— 大模型给出精确
    // 光标位置不可靠，统一按符号名经语言服务反查定位。
    ParamSchema file = makeParam("file", ParamType::String, "TypeScript/JavaScript 文件绝对路径");
    ParamSchema name = makeParam("name", ParamType::String, "符号名（类 / 函数 / 接口 / 变量 / 方法等）");

    switch (method)
    {
        case TsMethod::Source:
            def.name = "ts_source";
            def.description = "按符号名查找声明并返回源码正文 {file, line, endLine, source}。"
                             "import/export 别名自动解析到真正的定义文件。改代码前先用它看函数体 / "
                             "类定义，避免整读大文件。";
            def.parameters = {file, name,
                              makeParam("max_chars", ParamType::Number, "源码截断上限（默认 8000 字符）")};
            def.required_parameters = {"file", "name"};
            break;
        case TsMethod::References:
            def.name = "ts_references";
            def.description = "按符号名查找它在项目内的所有引用位置 [{file, line, text}]，"
                             "text 为引用所在行源码。修改接口 / 函数签名之前先查它评估影响面，"
                             "定义位置也包含在内。";
            def.parameters = {file, name};
            def.required_parameters = {"file", "name"};
            break;
        case TsMethod::QuickInfo:
            def.name = "ts_quickinfo";
            def.description = "按符号名查询其类型签名与文档注释 {name, contents}"
                             "（等价于编辑器悬停提示）。想确认某个符号的类型 / 参数 / 返回值时用它。";
            def.parameters = {file, name};
            def.required_parameters = {"file", "name"};
            break;
        case TsMethod::Diagnostics:
            def.name = "ts_diagnostics";
            def.description = "返回文件的语法 / 语义诊断 [{file, line, message, code, severity}]。"
                             "改完代码后用它确认没有引入类型错误。";
            def.parameters = {file};
            def.required_parameters = {"file"};
            break;
        case TsMethod::Outline:
            def.name = "ts_outline";
            def.description = "列出文件内全部符号的层级结构（类 / 函数 / 接口 / 方法 / 变量），"
                             "每项含 {name, kind, detail, line, endLine, children}。"
                             "先看文件结构，再决定读哪段或查哪个符号。";
            def.parameters = {file};
            def.required_parameters = {"file"};
            break;
        case TsMethod::Members:
            def.name = "ts_members";
            def.description = "按符号名列出其成员 {name, kind, detail, line, ...}：类 / 接口返回"
                             "属性与方法签名，模块 / 命名空间返回导出清单，函数返回调用签名。"
                             "了解一个类型有哪些成员时用它。";
            def.parameters = {file, name,
                              makeParam("max_members", ParamType::Number, "成员数量上限（默认 100）")};
            def.required_parameters = {"file", "name"};
            break;
        case TsMethod::WorkspaceSymbols:
            def.name = "ts_symbols";
            def.description = "在整个项目内按名称模糊搜索符号 [{name, kind, container, file, line}]。"
                             "不知道某个符号定义在哪个文件时先用它定位文件，"
                             "再用 ts_source / ts_members 深入查看。";
            def.parameters = {makeParam("query", ParamType::String, "符号名或其一部分（模糊匹配）"),
                              makeParam("limit", ParamType::Number, "返回数量上限（默认 30）")};
            def.required_parameters = {"query"};
            break;
        default:
            break; // Definition 仅供 ts_source 内部定位 / 自愈测试，不注册为工具
    }

    if (!def.name.empty())
        setToolDefine(def);
}

void JZTsTool::call(const ToolCall &params)
{
    const Json::Value &p = params.params;
    Json::Value req;

    if (m_method == TsMethod::WorkspaceSymbols)
    {
        string query = p.get("query", "").asString();
        if (query.empty())
        {
            finish(params, false, "参数 query 不能为空");
            return;
        }
        req["query"] = query;
        if (p.isMember("limit"))
            req["limit"] = p["limit"].asInt();
    }
    else
    {
        string file = p.get("file", "").asString();
        if (file.empty())
        {
            finish(params, false, "参数 file 不能为空");
            return;
        }
        req["file"] = file;

        // 位置类查询必须给符号名；错误信息同时给出下一步建议
        if (m_method != TsMethod::Diagnostics && m_method != TsMethod::Outline)
        {
            string sym = p.get("name", "").asString();
            if (sym.empty())
            {
                finish(params, false, "参数 name（符号名）不能为空。"
                       "不知道符号在哪个文件时可先用 ts_symbols(query) 搜索");
                return;
            }
            req["name"] = sym;
        }

        if (m_method == TsMethod::Source && p.isMember("max_chars"))
            req["max_chars"] = p["max_chars"].asInt();
        if (m_method == TsMethod::Members && p.isMember("max_members"))
            req["max_members"] = p["max_members"].asInt();
    }

    m_server->request(m_method, req, [this, params](bool ok, const string &payload) {
        this->finish(params, ok, payload);
    });
}
