#include "JZJSHttpModule.h"

#include "JZJSBind.h"

// C++ 标准结构，完全对齐 Node.js Fetch Response
struct FetchResponse
{
    int status = 0;          // HTTP 状态码 200/404/500
    std::string statusText;  // 标准文本
    bool ok = false;         // 200~299 = true
    std::string url;         // 请求地址
    std::string body;        // 响应原始数据
};

// JZJSHttpModule
JZJSHttpModule::JZJSHttpModule()
{
    m_net = new JZNetworkAccessManager(this);
    m_engine = nullptr;
}

JZJSHttpModule::~JZJSHttpModule()
{
    auto reply_map = m_replyMap;
    for (auto& pair : reply_map)
    {
        pair.first->close();
        pair.first->deleteLater();
    }
    m_replyMap.clear();
}

void JZJSHttpModule::initEngine(JZJSEngine* engine)
{
    m_engine = engine;
    auto global = m_engine->globalObject();

    // 标准属性
    jsbind::ClassBind<FetchResponse> fetch_res("FetchResponse");
    fetch_res.defProperty("status", &FetchResponse::status);
    fetch_res.defProperty("statusText", &FetchResponse::statusText);
    fetch_res.defProperty("ok", &FetchResponse::ok);
    fetch_res.defProperty("url", &FetchResponse::url);

    fetch_res.def("text",
                  [this](FetchResponse* res) -> JSValue
                  {
                      JZJSValuePtr ptr = m_engine->toValuePtr(res->body);
                      JSValue promise = m_engine->createResolvedPromise(ptr.get());
                      return promise;
                  });

    fetch_res.def("json",
                  [this](FetchResponse* res) -> JSValue
                  {
                      auto ctx = m_engine->context();
                      JSValue jsonVal = JS_ParseJSON(ctx, res->body.c_str(), res->body.size(), "json");
                      JSValue promise;
                      if (!JS_IsException(jsonVal))
                          promise = m_engine->createResolvedPromise(jsonVal);
                      else
                          promise = m_engine->createRejectPromise(jsonVal);
                      JS_FreeValue(ctx, jsonVal);
                      return promise;
                  });

    m_engine->registClass(fetch_res.classDefine());

    // 全局 fetch
    m_engine->registAsyncFunction(global, "fetch", jsbind::createSelfFunction(this, &JZJSHttpModule::fetch));
}

// ======================================================================
// 标准 fetch 实现
// ======================================================================
void JZJSHttpModule::fetch(int promiseHandle, const std::string& url, const Json::Value& options)
{
    if (url.empty())
    {
        m_engine->setPromiseReject(promiseHandle, "fetch: url required");
        return;
    }

    std::string method = options["method"].asString();
    if (method.empty())
        method = "GET";

    std::string body = options["body"].asString();
    JZNetworkRequest req = makeRequest(url, options);

    JZNetworkReply* reply = nullptr;
    if (method == "GET")
    {
        reply = m_net->get(req);
    }
    else if (method == "POST")
    {
        reply = m_net->post(req, str_toUtf8(body));
    }
    else
    {
        m_engine->setPromiseReject(promiseHandle, "unsupported method");
        return;
    }

    connect(&reply->sigFinished, this, &JZJSHttpModule::onNetFinish);
    m_replyMap[reply] = promiseHandle;
}

JZNetworkRequest JZJSHttpModule::makeRequest(const std::string& url, const Json::Value& options)
{
    JZNetworkRequest req;
    req.setUrl(url);

    if (options["headers"].isObject())
    {
        auto& headers = options["headers"];
        auto it = headers.begin();
        while (it != headers.end())
        {
            req.setRawHeader(it.key().asString(), (*it).asString());
            ++it;
        }
    }

    req.setRawHeader("User-Agent", "node-fetch");
    return req;
}

// ======================================================================
// 关键：返回标准 Response 对象，支持 .text() .json()
// ======================================================================
void JZJSHttpModule::onNetFinish()
{
    JZNetworkReply* reply = (JZNetworkReply*)(sender());
    reply->deleteLater();

    auto it = m_replyMap.find(reply);
    if (it == m_replyMap.end())
    {
        return;
    }

    int promiseHandle = it->second;
    m_replyMap.erase(it);

    // 构建标准 Response 对象
    Json::Value res;
    if (reply->error() == JZNetworkReply::NoError || reply->error() == JZNetworkReply::HttpError)
    {
        std::string rawText = str_fromUtf8(reply->readAll());

        FetchResponse* fetch_res = new FetchResponse();
        fetch_res->ok = (reply->error() == JZNetworkReply::NoError);
        fetch_res->status = reply->httpStatusCode();
        fetch_res->statusText = reply->httpStatusCodeText();
        fetch_res->body = rawText;

        int res_type = m_engine->ctypeidToType(typeid(FetchResponse).name());
        auto js_res = m_engine->createValue(res_type, fetch_res);
        JZJSValuePtr response = m_engine->makeValuePtr(js_res);

        // 成功返回 Response 对象
        std::vector<JZJSValuePtr> args = {response};
        m_engine->setPromiseResolve(promiseHandle, args);
    }
    else
    {
        m_engine->setPromiseReject(promiseHandle, reply->errorString());
    }
}