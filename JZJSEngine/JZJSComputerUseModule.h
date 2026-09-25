#pragma once
#include "JZJSEngine.h"
#include "JZComputerUse.h"
#include "json/json.h"

// 把 JZComputerUse 的底层语义暴露给 JS: __jz.computeruse.*
// 接入方式(构建时补): 在 JZEngineRuntime.cpp 里
//   m_computerUseModule = std::make_unique<JZJSComputerUseModule>(); m_computerUseModule->initEngine(engine);
class JZJSComputerUseModule : public JZObject {
public:
    void initEngine(JZJSEngine* engine);
    void deinitEngine();

private:
    JZJSEngine* m_engine = nullptr;
    static JZComputerUse::MouseBtn btnFromStr(const std::string& s);

    // 桥接方法(参数/返回值由 jsbind 按 C++ 签名自动转换; Json::Value 自动转 JS 对象)
    void        jsSwitchToDesktop();
    bool        jsClick(double x, double y, const std::string& btn);
    bool        jsDoubleClick(double x, double y, const std::string& btn);
    bool        jsDrag(double x1, double y1, double x2, double y2);
    void        jsKey(const std::string& name);
    void        jsType(const std::string& text);
    void        jsWait(int ms);
    std::string jsScreenshot();
    Json::Value jsMatch(const std::string& locatorJson);
};
