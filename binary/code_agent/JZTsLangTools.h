#ifndef JZ_TS_LANG_TOOLS_H_
#define JZ_TS_LANG_TOOLS_H_

#include "JZCodeTools.h"
#include "JZTsLangServer.h"

// 直连 TSGO 语言服务的 agent 工具。
// 每个实例对应一个查询类型（TsMethod，见 JZTsLangServer.h）：call() 把参数
// （file / line / offset）透传给 JZTsLangServer，由后者经原生 TSGO LSP 服务器
// （tsc.exe --lsp --stdio）完成查询，结果回传后通过 sigResult 交给 agent。
// 不再经过 MCP / HTTP / node 包装层。
class JZTsTool : public JZCodeTool
{
public:
    JZTsTool(JZTsLangServer *server, TsMethod method);
    virtual void call(const ToolCall &params) override;

private:
    JZTsLangServer *m_server = nullptr;
    TsMethod m_method;
};

#endif // !JZ_TS_LANG_TOOLS_H_
