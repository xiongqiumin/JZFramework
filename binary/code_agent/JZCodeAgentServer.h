#ifndef JZ_CODE_AGENT_SERVER_H_
#define JZ_CODE_AGENT_SERVER_H_

#include <string>

#include "JZHttpHandler.h"
#include "JZHttpServer.h"
#include "JZHttpStaticFiles.h"
#include "JZCodeAgent.h"
#include "JZSpeechClient.h"

// 把 JZCodeAgent 包装成一个 HTTP 服务：
//   /                 网页（编译进二进制的资源）
//   GET  /api/state   增量拉取会话日志
//   POST /api/chat    提交任务
//   POST /api/stop    停止当前任务
//   POST /api/clear   清空会话
//   GET/POST /api/config  读写配置
//   GET  /api/files   浏览工作区目录
//   GET  /api/file    读取工作区文件
class JZCodeAgentServer : public JZObject
{
public:
    JZCodeAgentServer(JZObject *parent = nullptr);
    ~JZCodeAgentServer();

    bool start(JZCodeAgent *agent, int port);
    int port() const;
    void setAgent(JZCodeAgent *agent) { m_agent = agent; }   // loadConfig 需在 start 前先绑定

    // 配置本地持久化 (网页读写 /api/config 会落到这个文件)
    void setConfigPath(const std::string &path) { m_configPath = path; }
    bool loadConfig();   // 启动时读文件, 应用到 agent 与本对象
    bool saveConfig();   // 当前配置写回文件

    // 语音客户端 (对接 ugreen-server 的 ASR/TTS); 为空则 /api/asr /api/tts 不可用
    void setSpeechClient(JZSpeechClient *speech) { m_speech = speech; }
    JZSpeechClient *speechClient() const { return m_speech; }
    const std::string &speechWs() const { return m_speechWs; }
    bool localPlay() const { return m_localPlay; }

    // 由 JZCodeAgentApiHandler 回调
    void handleApi(JZHttpServerClient *client, const JZHttpRequest &req, const std::string &route);

protected:
    void replyJson(JZHttpServerClient *client, int code, const Json::Value &root);
    void replyError(JZHttpServerClient *client, int code, const std::string &msg);
    bool parseBody(const JZHttpRequest &req, Json::Value &root, std::string &error);

    void handleState(JZHttpServerClient *client, const JZHttpRequest &req);
    void handleChat(JZHttpServerClient *client, const JZHttpRequest &req);
    void handleStop(JZHttpServerClient *client, const JZHttpRequest &req);
    void handleClear(JZHttpServerClient *client, const JZHttpRequest &req);
    void handleConfig(JZHttpServerClient *client, const JZHttpRequest &req);
    void handleFiles(JZHttpServerClient *client, const JZHttpRequest &req);
    void handleFile(JZHttpServerClient *client, const JZHttpRequest &req);
    void handleAsr(JZHttpServerClient *client, const JZHttpRequest &req);
    void handleTts(JZHttpServerClient *client, const JZHttpRequest &req);
    // 浏览器默认会请求 /favicon.ico，这里回内嵌的 svg 图标，避免打到静态资源上报错
    void handleFavicon(JZHttpServerClient *client);

    JZHttpServer *m_server = nullptr;
    JZCodeAgent *m_agent = nullptr;
    JZSpeechClient *m_speech = nullptr;
    int m_port = 18080;
    std::string m_configPath;                       // 本地配置文件 (json)
    std::string m_speechWs = "ws://127.0.0.1:8000"; // ugreen-server WS 地址
    bool m_localPlay = false;                       // 本地(JZAudio)播放 TTS
};

// 把 HTTP 请求转发给 JZCodeAgentServer::handleApi 的薄壳 handler
class JZCodeAgentApiHandler : public JZHttpHandler
{
public:
    JZCodeAgentServer *server = nullptr;
    std::string route;

    virtual void handle(JZHttpServerClient *client, const JZHttpRequest &req) override;
};

#endif // !JZ_CODE_AGENT_SERVER_H_
