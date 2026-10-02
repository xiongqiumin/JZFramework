#pragma once
#include "JZJSEngine.h"
#include "JZComputerUse/JZComputerUse.h"   // 路径带前缀: 该目录不在 include 搜里(顶层模块目录)
#include "json/json.h"

// 把 JZComputerUse 的底层语义暴露给 JS: __jz.computeruse.*
// 接入方式(构建时补): 在 JZEngineRuntime.cpp 里
//   m_computerUseModule = std::make_unique<JZJSComputerUseModule>(); m_computerUseModule->initEngine(engine);
class JZFRAMEWORK_API JZJSComputerUseModule : public JZObject {
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
    // —— 布局通道（运行期寻址的正路；见 docs/compute-use-design.md §4.1）——
    Json::Value jsLayoutTree(const std::string& optsJson); // 可访问布局树(归一化 rect); 可指定目标 title
    bool        jsWaitStable(const std::string& optsJson); // 等界面稳定(像素只当信号)
    bool        jsLaunch(const std::string& optsJson);     // 启动 / 激活目标窗口

    // —— OCR 兜底(像素族里的"文字"那条; 见 docs/compute-use-design.md §4.1) ——
    bool        jsInitOcr(const std::string& optsJson);    // { modelRoot? } -> 是否就绪
    bool        jsOcrReady();
    Json::Value jsOcrTexts(const std::string& optsJson);   // { region?, minScore? } -> [{text,score,x,y,w,h}]
    // 能力探测 / 诊断 / 释放: "有没有 OCR 能力"与"模型好没好"是两件事, 分开问才不会误判
    bool        jsOcrBuilt();                              // 本构建是否编入了 OCR(JZ_CU_OCR)
    Json::Value jsOcrInfo();                               // {built, ready, cls, modelRoot, hint?}
    void        jsDeinitOcr();                             // 释放模型占的内存
};
