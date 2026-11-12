#ifndef JZ_AGENT_HUB_PACKET_H_
#define JZ_AGENT_HUB_PACKET_H_

#include "JZNet/JZNetPacket.h"

// ⚠ 位置: 同 JZAgentNodePacket.h —— 只服务 agent_cli 这一族 exe, 不属于 JZFramework
//   的导出面, 编进静态库 agent_cli_packet; 因此这里的类和函数**不带** JZFRAMEWORK_API。
// ---------------------------------------------------------------------------
// agent_cli <-> agent_cli_server 的控制面协议(cli 是 client, server 是 server)
//
// 设计见 docs/agent-hub-design.md §4:
//   - cli **主动**连 server(出站), 所以内网机器不用 frp / 不用端口映射;
//   - 整条链路上 cli 永远是 client: 控制面连 server, 执行面连 node;
//   - 业务载荷(prompt/验证参数/事件/回包)原样放在 **JSON 字符串**里, 与现有
//     POST /task 的字段完全一致 —— 这样 cli 侧直接复用 submitTask(), server 侧
//     不需要跟着 agent 的字段变化改协议(加字段只改 JSON, 不动包结构)。
//
// ⚠ 单包上限 MG_MAX_PACK_SIZE(256KB): 事件里的工具输出必须两端都裁, 否则
//   一次大编译日志会把连接打成畸形包(见 JZHubLink 里的 capText)。
// ---------------------------------------------------------------------------
enum
{
    // 1000/1001~ 给 node 执行面(JZAgentNodePacket), 控制面从 1101 起, 互不冲突
    HubPack_hello = NetPack_user + 101,   // cli -> server: 上线自报 + 节点清单
    HubPack_helloAck,                     // server -> cli: 接受/拒绝 + 服务器时间
    HubPack_heartbeat,                    // cli -> server: 5s 一次(忙闲/轮次/节点状态)
    HubPack_taskSubmit,                   // server -> cli: 提交一个任务(body = /task 的 JSON)
    HubPack_taskCancel,                   // server -> cli: 取消一个在跑的任务
    HubPack_reset,                        // server -> cli: 重置(scope = task / history)
    HubPack_taskEvent,                    // cli -> server: 任务事件(thinking/reply/tool_call/...)
    HubPack_taskEnd,                      // cli -> server: 任务回包(status/agent_reply/...)
};

// cli -> server: 上线
class JZHubPackHello : public JZNetPack
{
public:
    virtual int packetType() const;
    virtual void saveToStream(JZDataStream &s) const;
    virtual void loadFromStream(JZDataStream &s);

    std::string agentId;      // server 侧登记的身份(配置 server.agentId, 空则用 agentName)
    std::string name;         // 展示名
    std::string version;      // cli 版本
    std::string rootDir;      // 工作区(管理台展示/排障)
    std::string project;      // 项目名(一个 cli 一个 project, 网页按它筛选)
    std::string modelName;    // 当前模型(不传 token)
    std::string nodes;        // 节点清单 JSON 数组: [{name,target,online,busy,nodeName,rootDir,shell,error}]
};

// server -> cli: hello 的回应
class JZHubPackHelloAck : public JZNetPack
{
public:
    JZHubPackHelloAck();

    virtual int packetType() const;
    virtual void saveToStream(JZDataStream &s) const;
    virtual void loadFromStream(JZDataStream &s);

    bool ok;
    std::string error;        // ok=false 时给原因(token 不对 / agentId 未登记 / 维护模式)
    bool acceptTasks = true;  // false = 只上报不接任务(维护模式)
    int64_t serverTimeMs = 0;
};

// cli -> server: 心跳 + 状态快照
class JZHubPackHeartbeat : public JZNetPack
{
public:
    JZHubPackHeartbeat();

    virtual int packetType() const;
    virtual void saveToStream(JZDataStream &s) const;
    virtual void loadFromStream(JZDataStream &s);

    bool busy = false;
    int round = 0;
    std::string taskId;
    std::string nodes;        // 同 hello.nodes(节点上下线/忙闲变化靠它刷新)
};

// server -> cli: 提交任务；body 就是 POST /task 的请求体
class JZHubPackTaskSubmit : public JZNetPack
{
public:
    virtual int packetType() const;
    virtual void saveToStream(JZDataStream &s) const;
    virtual void loadFromStream(JZDataStream &s);

    std::string taskId;       // server 生成, 回包任务用
    std::string body;         // JSON
};

// server -> cli: 取消任务(等价 POST /reset scope=task)
class JZHubPackTaskCancel : public JZNetPack
{
public:
    virtual int packetType() const;
    virtual void saveToStream(JZDataStream &s) const;
    virtual void loadFromStream(JZDataStream &s);

    std::string taskId;
};

// server -> cli: 重置(scope = task / history)
class JZHubPackReset : public JZNetPack
{
public:
    virtual int packetType() const;
    virtual void saveToStream(JZDataStream &s) const;
    virtual void loadFromStream(JZDataStream &s);

    std::string scope = "task";
};

// cli -> server: 任务事件(type/data 与 SSE 的取值完全一致)
class JZHubPackTaskEvent : public JZNetPack
{
public:
    virtual int packetType() const;
    virtual void saveToStream(JZDataStream &s) const;
    virtual void loadFromStream(JZDataStream &s);

    std::string taskId;
    std::string type;         // thinking / reply / tool_call / tool_result / verify / error / task_start / task_end ...
    std::string data;         // JSON
};

// cli -> server: 任务回包(与 POST /task 的回包一致)
class JZHubPackTaskEnd : public JZNetPack
{
public:
    virtual int packetType() const;
    virtual void saveToStream(JZDataStream &s) const;
    virtual void loadFromStream(JZDataStream &s);

    std::string taskId;
    std::string result;       // JSON: {status, verify_rounds, agent_reply, last_verify_output, error?}
};

// 注册全部控制面包(幂等; 必须在 JZNetPackInit 之后调用, 两边进程各调一次)
void JZAgentHubPacketInit();

#endif // !JZ_AGENT_HUB_PACKET_H_
