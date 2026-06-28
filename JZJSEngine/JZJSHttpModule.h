#ifndef JS_ENGINE_NET_H_
#define JS_ENGINE_NET_H_

#include <map>

#include "JZJSEngine.h"
#include "JZNetworkAccessManager.h"
#include "json/json.h"

class JZJSHttpModule : public JZObject
{
public:
    JZJSHttpModule();
    ~JZJSHttpModule();

    void initEngine(JZJSEngine* engine);
    void fetch(int promiseHandle, const std::string& url, const Json::Value& options);

protected:
    void onNetFinish();

    JZNetworkRequest makeRequest(const std::string& url, const Json::Value& options);

    JZNetworkAccessManager* m_net;
    JZJSEngine* m_engine;
    std::map<JZNetworkReply*, int> m_replyMap;
};

#endif  // !JS_ENGINE_NET_H_
