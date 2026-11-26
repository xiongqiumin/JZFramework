#include "JZServerExtraTools.h"

#include "JZAgentCliServer.h"
#include "JZCliExtraTool.h"
#include "JZCodeTools.h"
#include "JZThread.h"

#include <functional>
#include <string>

namespace {

static ParamSchema makeProxyParam(const std::string &name, ParamType type,
                                  const std::string &description)
{
    ParamSchema p;
    p.name = name;
    p.type = type;
    p.description = description;
    return p;
}

// 工具实例归 cli 线程; server 状态归 server 线程。call() 只投递请求, 不阻塞等待。
class JZServerToolProxy : public JZAgentTool
{
public:
    JZServerToolProxy(JZAgentCliServer *server, JZObject *)
        : JZAgentTool()
        , m_server(server)
    {
    }

    void cancel(const std::string &) override { m_cancelled = true; }

protected:
    JZAgentCliServer *m_server = nullptr;

    void dispatch(const ToolCall &params,
                  const std::function<void(bool &, std::string &)> &work)
    {
        if (!m_server) {
            finish(params, false, "server 不可用");
            return;
        }

        m_activeId = params.id;
        m_cancelled = false;

        JZObjectPointer selfGuard = guardPointer();
        JZObjectPointer serverGuard = m_server->guardPointer();
        ToolCall call = params;

        runOnObject(m_server, [selfGuard, serverGuard, call, work]() {
            if (!selfGuard || !serverGuard)
                return;

            bool ok = false;
            std::string result;
            work(ok, result);

            runOnObject(selfGuard, [selfGuard, call, ok, result]() {
                if (!selfGuard)
                    return;
                JZServerToolProxy *self =
                    static_cast<JZServerToolProxy *>(selfGuard.object());
                if (!self || self->m_cancelled || self->m_activeId != call.id)
                    return;

                ToolCallResult r;
                r.id = call.id;
                r.isOk = ok;
                r.result = result;
                self->sigResult.emit(r);
            });
        });
    }

    void finish(const ToolCall &params, bool ok, const std::string &result)
    {
        ToolCallResult r;
        r.id = params.id;
        r.isOk = ok;
        r.result = result;
        sigResult.emit(r);
    }

private:
    std::string m_activeId;
    bool m_cancelled = false;
};

class JZServerAgentsProxyTool : public JZServerToolProxy
{
public:
    explicit JZServerAgentsProxyTool(JZAgentCliServer *server, JZObject *parent)
        : JZServerToolProxy(server, parent)
    {
        ToolDefine def;
        def.name = "server_agents";
        def.description =
            "列出这个集群里的所有 agent(在线/忙闲/节点/当前任务/工作区)与任务计数。"
            "给某台派任务前先用它确认 agentId。";
        setToolDefine(def);
    }

    void call(const ToolCall &params) override
    {
        JZAgentCliServer *server = m_server;
        dispatch(params, [server](bool &ok, std::string &result) {
            if (!server) {
                ok = false;
                result = "server 不可用";
                return;
            }
            ok = true;
            result = server->snapshotAgents().toStyledString();
        });
    }
};

class JZServerTaskQueryProxyTool : public JZServerToolProxy
{
public:
    explicit JZServerTaskQueryProxyTool(JZAgentCliServer *server, JZObject *parent)
        : JZServerToolProxy(server, parent)
    {
        ToolDefine def;
        def.name = "server_task_query";
        def.description =
            "查 A2A Task。不给 task_id 时按时间倒序列出最近若干条(可按 agent_id / state 过滤);"
            "给 task_id 时返回那一条的详情。";
        def.parameters.push_back(makeProxyParam("task_id", ParamType::String, "Task id(留空 = 列表)"));
        def.parameters.push_back(makeProxyParam("agent_id", ParamType::String, "只看某个 agent(可选)"));
        def.parameters.push_back(makeProxyParam("state", ParamType::String, "只看某个 A2A TaskState(可选)"));
        def.parameters.push_back(makeProxyParam("limit", ParamType::Number, "最多返回多少条, 默认 20"));
        setToolDefine(def);
    }

    void call(const ToolCall &params) override
    {
        JZAgentCliServer *server = m_server;
        dispatch(params, [server, params](bool &ok, std::string &result) {
            if (!server) {
                ok = false;
                result = "server 不可用";
                return;
            }

            const std::string taskId = params.params.get("task_id", "").asString();
            if (!taskId.empty()) {
                Json::Value one = server->snapshotTask(taskId);
                if (one.isNull()) {
                    ok = false;
                    result = "没有这个任务: " + taskId;
                    return;
                }
                ok = true;
                result = one.toStyledString();
                return;
            }

            int limit = params.params.get("limit", 20).asInt();
            if (limit <= 0 || limit > 100)
                limit = 20;
            Json::Value list = server->snapshotTasks(
                params.params.get("agent_id", "").asString(),
                params.params.get("state", "").asString(), limit);
            ok = true;
            result = list.toStyledString();
        });
    }
};

} // namespace

void jzRegisterServerExtraTools(JZAgentCliServer *server)
{
    jzRegisterCliExtraTool("server_agents", [server](JZObject *parent) -> JZAgentTool * {
        return new JZServerAgentsProxyTool(server, parent);
    });
    jzRegisterCliExtraTool("server_task_query", [server](JZObject *parent) -> JZAgentTool * {
        return new JZServerTaskQueryProxyTool(server, parent);
    });
}

void jzUnregisterServerExtraTools()
{
    jzUnregisterCliExtraTool("server_agents");
    jzUnregisterCliExtraTool("server_task_query");
}
