#include "JZCodeAgentServer.h"

#include "jz_code_agent_resources.h"

#include "JZByteArray.h"
#include "JZDebug.h"
#include "JZResources.h"
#include "JZStd.h"

using namespace std;

static Json::Value makeOk()
{
    Json::Value root;
    root["ok"] = true;
    return root;
}

// ==================== JZCodeAgentApiHandler ====================
void JZCodeAgentApiHandler::handle(JZHttpServerClient *client, const JZHttpRequest &req)
{
    if (server)
        server->handleApi(client, req, route);
}

// ==================== JZCodeAgentServer ====================
JZCodeAgentServer::JZCodeAgentServer(JZObject *parent)
    : JZObject(parent)
{
}

JZCodeAgentServer::~JZCodeAgentServer()
{
}

bool JZCodeAgentServer::start(JZCodeAgent *agent, int port)
{
    m_agent = agent;
    m_port = port;

    // 注册编译进二进制的网页资源（:/code_agent/...）
    static bool s_resInited = false;
    if (!s_resInited)
    {
        jzres_code_agent_init();
        s_resInited = true;
    }

    m_server = new JZHttpServer(this);

    JZHttpStaticFiles *web = new JZHttpStaticFiles(":/code_agent");
    m_server->addRoute("/", web);
    m_server->addRoute("/<path:path>", web);

    auto addApi = [this](const string &path)
    {
        JZCodeAgentApiHandler *h = new JZCodeAgentApiHandler();
        h->server = this;
        h->route = path;
        m_server->addRoute(path, h);
    };
    addApi("/api/state");
    addApi("/api/chat");
    addApi("/api/stop");
    addApi("/api/clear");
    addApi("/api/config");
    addApi("/api/files");
    addApi("/api/file");
    addApi("/favicon.ico");

    if (!m_server->listen(port))
    {
        jzError() << "监听端口失败: " << port;
        return false;
    }
    return true;
}

int JZCodeAgentServer::port() const
{
    return m_port;
}

void JZCodeAgentServer::replyJson(JZHttpServerClient *client, int code, const Json::Value &root)
{
    Json::FastWriter writer;
    string body = writer.write(root);

    JZHttpRespone resp;
    resp.setStatusCode(code);
    resp.setJson(body);
    resp.setHeader("Access-Control-Allow-Origin", "*");
    resp.setHeader("Access-Control-Allow-Headers", "Content-Type");
    resp.setHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    resp.setHeader("Cache-Control", "no-store");
    client->response(resp.toByteArray());
}

void JZCodeAgentServer::replyError(JZHttpServerClient *client, int code, const string &msg)
{
    Json::Value root;
    root["ok"] = false;
    root["error"] = msg;
    replyJson(client, code, root);
}

bool JZCodeAgentServer::parseBody(const JZHttpRequest &req, Json::Value &root, string &error)
{
    Json::Reader reader;
    string body = str_fromUtf8(req.body());
    if (body.empty())
        body = "{}";
    if (!reader.parse(body, root))
    {
        error = "请求体不是合法 json: " + reader.getFormattedErrorMessages();
        return false;
    }
    return true;
}

void JZCodeAgentServer::handleApi(JZHttpServerClient *client, const JZHttpRequest &req, const string &route)
{
    if (req.methodString() == "OPTIONS")
    {
        replyJson(client, 200, makeOk());
        return;
    }

    if (route == "/api/state")
        handleState(client, req);
    else if (route == "/api/chat")
        handleChat(client, req);
    else if (route == "/api/stop")
        handleStop(client, req);
    else if (route == "/api/clear")
        handleClear(client, req);
    else if (route == "/api/config")
        handleConfig(client, req);
    else if (route == "/api/files")
        handleFiles(client, req);
    else if (route == "/api/file")
        handleFile(client, req);
    else if (route == "/favicon.ico")
        handleFavicon(client);
    else
        replyError(client, 404, "未知接口: " + route);
}

void JZCodeAgentServer::handleState(JZHttpServerClient *client, const JZHttpRequest &req)
{
    int since = str_toInt(req.queryParam("since"));

    Json::Value logs(Json::arrayValue);
    for (const auto &log : m_agent->logs())
    {
        if (log.id > since)
            logs.append(log.toJson());
    }

    Json::Value usage;
    JZAgentTokenUsage u = m_agent->usage();
    usage["prompt_tokens"] = u.prompt_tokens;
    usage["completion_tokens"] = u.completion_tokens;
    usage["total_tokens"] = u.total_tokens;

    Json::Value tools(Json::arrayValue);
    for (const auto &name : m_agent->toolNames())
        tools.append(name);

    Json::Value root;
    root["busy"] = m_agent->isBusy();
    root["seq"] = m_agent->logSeq();
    root["logs"] = logs;
    root["usage"] = usage;
    root["tools"] = tools;
    root["workspace"] = m_agent->codeConfig().workspace;
    replyJson(client, 200, root);
}

void JZCodeAgentServer::handleChat(JZHttpServerClient *client, const JZHttpRequest &req)
{
    Json::Value body;
    string error;
    if (!parseBody(req, body, error))
    {
        replyError(client, 400, error);
        return;
    }

    string message = body.get("message", "").asString();
    if (str_trimmed(message).empty())
    {
        replyError(client, 400, "message 不能为空");
        return;
    }
    if (m_agent->isBusy())
    {
        replyError(client, 409, "上一个任务还在执行中，请等待或点“停止”");
        return;
    }

    m_agent->request(message);
    replyJson(client, 200, makeOk());
}

void JZCodeAgentServer::handleStop(JZHttpServerClient *client, const JZHttpRequest &req)
{
    m_agent->stop();
    replyJson(client, 200, makeOk());
}

void JZCodeAgentServer::handleClear(JZHttpServerClient *client, const JZHttpRequest &req)
{
    m_agent->clearHistory();
    replyJson(client, 200, makeOk());
}

void JZCodeAgentServer::handleConfig(JZHttpServerClient *client, const JZHttpRequest &req)
{
    if (req.methodString() == "GET")
    {
        Json::Value root = makeOk();
        root["modelUrl"] = m_agent->llmConfig().modelUrl;
        root["modelName"] = m_agent->llmConfig().modelName;
        root["modelToken"] = m_agent->llmConfig().modelToken;
        root["workspace"] = m_agent->codeConfig().workspace;
        root["buildCommand"] = m_agent->codeConfig().buildCommand;
        root["runCommand"] = m_agent->codeConfig().runCommand;
        replyJson(client, 200, root);
        return;
    }

    Json::Value body;
    string error;
    if (!parseBody(req, body, error))
    {
        replyError(client, 400, error);
        return;
    }

    if (body.isMember("buildCommand"))
        m_agent->setBuildCommand(body["buildCommand"].asString());
    if (body.isMember("runCommand"))
        m_agent->setRunCommand(body["runCommand"].asString());
    if (body.isMember("workspace"))
    {
        string ws = body["workspace"].asString();
        if (!ws.empty() && !m_agent->setWorkspace(ws))
        {
            replyError(client, 400, "工作区目录不存在: " + ws);
            return;
        }
    }

    JZAgentConfig llm = m_agent->llmConfig();
    bool llmChanged = false;
    if (body.isMember("modelUrl"))
    {
        llm.modelUrl = body["modelUrl"].asString();
        llmChanged = true;
    }
    if (body.isMember("modelName"))
    {
        llm.modelName = body["modelName"].asString();
        llmChanged = true;
    }
    if (body.isMember("modelToken"))
    {
        llm.modelToken = body["modelToken"].asString();
        llmChanged = true;
    }
    if (llmChanged)
        m_agent->applyLlmConfig(llm);

    replyJson(client, 200, makeOk());
}

void JZCodeAgentServer::handleFiles(JZHttpServerClient *client, const JZHttpRequest &req)
{
    string rel = req.queryParam("path");

    vector<string> items;
    string error;
    if (!m_agent->listFiles(rel, items, error))
    {
        replyError(client, 400, error);
        return;
    }

    Json::Value arr(Json::arrayValue);
    for (const auto &name : items)
    {
        bool isDir = str_endsWith(name, "/");
        Json::Value item;
        item["name"] = isDir ? name.substr(0, name.size() - 1) : name;
        item["isDir"] = isDir;
        arr.append(item);
    }

    Json::Value root = makeOk();
    root["path"] = rel;
    root["items"] = arr;
    replyJson(client, 200, root);
}

void JZCodeAgentServer::handleFile(JZHttpServerClient *client, const JZHttpRequest &req)
{
    string rel = req.queryParam("path");
    string text, error;
    if (!m_agent->readFileText(rel, text, error))
    {
        replyError(client, 400, error);
        return;
    }

    const int kMaxChars = 200000;
    bool truncated = false;
    if ((int)text.size() > kMaxChars)
    {
        text = text.substr(0, kMaxChars);
        truncated = true;
    }

    Json::Value root = makeOk();
    root["path"] = rel;
    root["content"] = text;
    root["truncated"] = truncated;
    replyJson(client, 200, root);
}

void JZCodeAgentServer::handleFavicon(JZHttpServerClient *client)
{
    const JZResourceData *res = JZResources::instance()->find(":/code_agent/favicon.svg");

    JZHttpRespone resp;
    if (!res)
    {
        // 资源缺失时也不要回 404 打到静态 handler 上，直接告诉浏览器没有图标
        resp.setStatusCode(204);
        resp.setHeader("Cache-Control", "no-store");
        client->response(resp.toByteArray());
        return;
    }

    resp.setStatusCode(200);
    resp.setHeader("Content-Type", "image/svg+xml");
    resp.setHeader("Cache-Control", "public, max-age=86400");
    resp.setBody(JZByteArray((const char *)res->data, res->size));
    client->response(resp.toByteArray());
}
