#include "JZJSComputerUseModule.h"
#include "JZJSBind.h"                       // jsbind::createSelfFunction(桥接函数注册)
#include "JZComputerUse/JZComputerUse.h"
#include <json/json.h>

void JZJSComputerUseModule::initEngine(JZJSEngine* engine) {
    m_engine = engine;
    JSValue global = m_engine->globalObject();

    // 复用已存在的 __jz(不要覆盖其下的 core/net/... 子命名空间)
    JSValue jz = m_engine->getProperty(global, "__jz");
    bool fromGet = !JS_IsUndefined(jz);
    if (!fromGet) {
        jz = m_engine->createObject();
        m_engine->setProperty(global, "__jz", jz);
    }

    JSValue ns = m_engine->createObject();
    m_engine->registFunction(ns, "switchToDesktop", jsbind::createSelfFunction(this, &JZJSComputerUseModule::jsSwitchToDesktop));
    m_engine->registFunction(ns, "click",          jsbind::createSelfFunction(this, &JZJSComputerUseModule::jsClick));
    m_engine->registFunction(ns, "doubleClick",    jsbind::createSelfFunction(this, &JZJSComputerUseModule::jsDoubleClick));
    m_engine->registFunction(ns, "drag",           jsbind::createSelfFunction(this, &JZJSComputerUseModule::jsDrag));
    m_engine->registFunction(ns, "key",            jsbind::createSelfFunction(this, &JZJSComputerUseModule::jsKey));
    m_engine->registFunction(ns, "type",           jsbind::createSelfFunction(this, &JZJSComputerUseModule::jsType));
    m_engine->registFunction(ns, "wait",           jsbind::createSelfFunction(this, &JZJSComputerUseModule::jsWait));
    m_engine->registFunction(ns, "screenshot",     jsbind::createSelfFunction(this, &JZJSComputerUseModule::jsScreenshot));
    m_engine->registFunction(ns, "screenInfo",     jsbind::createSelfFunction(this, &JZJSComputerUseModule::jsScreenInfo));
    m_engine->registFunction(ns, "match",          jsbind::createSelfFunction(this, &JZJSComputerUseModule::jsMatch));
    // 进程 / 窗口管理（两个后端同一套语义: 真桌面 = EnumWindows/SetWindowPos, 虚拟桌面 = JZVirtualDevice）——
    // 脚本"像人一样操作这台小电脑"要用的就是这些: 把目标窗口带到前台、看窗口在哪多大、摆窗口。
    m_engine->registFunction(ns, "focusWindow",      jsbind::createSelfFunction(this, &JZJSComputerUseModule::jsFocusWindow));
    m_engine->registFunction(ns, "focusProcess",     jsbind::createSelfFunction(this, &JZJSComputerUseModule::jsFocusProcess));
    m_engine->registFunction(ns, "terminateProcess", jsbind::createSelfFunction(this, &JZJSComputerUseModule::jsTerminateProcess));
    m_engine->registFunction(ns, "windows",          jsbind::createSelfFunction(this, &JZJSComputerUseModule::jsWindows));
    m_engine->registFunction(ns, "windowState",      jsbind::createSelfFunction(this, &JZJSComputerUseModule::jsWindowState));
    // 布局通道(运行期寻址的正路): 导出可访问布局树 / 等界面稳定 / 启动激活目标窗口
    m_engine->registFunction(ns, "layoutTree",     jsbind::createSelfFunction(this, &JZJSComputerUseModule::jsLayoutTree));
    m_engine->registFunction(ns, "waitStable",     jsbind::createSelfFunction(this, &JZJSComputerUseModule::jsWaitStable));
    m_engine->registFunction(ns, "launch",         jsbind::createSelfFunction(this, &JZJSComputerUseModule::jsLaunch));
    // OCR 兜底(本地 PP-OCR): match({kind:'ocr',text}) 的后端 + 直接列屏上文字
    m_engine->registFunction(ns, "initOcr",        jsbind::createSelfFunction(this, &JZJSComputerUseModule::jsInitOcr));
    m_engine->registFunction(ns, "ocrReady",       jsbind::createSelfFunction(this, &JZJSComputerUseModule::jsOcrReady));
    m_engine->registFunction(ns, "ocrTexts",       jsbind::createSelfFunction(this, &JZJSComputerUseModule::jsOcrTexts));
    m_engine->registFunction(ns, "ocrBuilt",       jsbind::createSelfFunction(this, &JZJSComputerUseModule::jsOcrBuilt));
    m_engine->registFunction(ns, "ocrInfo",        jsbind::createSelfFunction(this, &JZJSComputerUseModule::jsOcrInfo));
    m_engine->registFunction(ns, "deinitOcr",      jsbind::createSelfFunction(this, &JZJSComputerUseModule::jsDeinitOcr));
    m_engine->setProperty(jz, "computeruse", ns);

    // 释放 getProperty 多 dup 的引用, 避免 engine 析构时 gc_obj_list 非空断言
    if (fromGet) JS_FreeValue(m_engine->context(), jz);
}

void JZJSComputerUseModule::deinitEngine() {
    m_engine = nullptr;
}

JZComputerUse::MouseBtn JZJSComputerUseModule::btnFromStr(const std::string& s) {
    if (s == "right")  return JZComputerUse::MouseBtn::Right;
    if (s == "middle") return JZComputerUse::MouseBtn::Middle;
    return JZComputerUse::MouseBtn::Left;
}

void JZJSComputerUseModule::jsSwitchToDesktop() { JZComputerUse::switchToDesktop(); }
bool JZJSComputerUseModule::jsClick(double x, double y, const std::string& btn) { return JZComputerUse::click(x, y, btnFromStr(btn)); }
bool JZJSComputerUseModule::jsDoubleClick(double x, double y, const std::string& btn) { return JZComputerUse::doubleClick(x, y, btnFromStr(btn)); }
bool JZJSComputerUseModule::jsDrag(double x1, double y1, double x2, double y2) { return JZComputerUse::drag(x1, y1, x2, y2); }
void JZJSComputerUseModule::jsKey(const std::string& name) { JZComputerUse::key(name); }
void JZJSComputerUseModule::jsType(const std::string& text) { JZComputerUse::type(text); }
void JZJSComputerUseModule::jsWait(int ms) { JZComputerUse::wait(ms); }
std::string JZJSComputerUseModule::jsScreenshot() { return JZComputerUse::screenshot(); }
Json::Value JZJSComputerUseModule::jsScreenInfo() { return JZComputerUse::screenInfo(); }

Json::Value JZJSComputerUseModule::jsMatch(const std::string& locatorJson) {
    Json::Value root; Json::Reader reader;
    if (!reader.parse(locatorJson, root)) return Json::Value();

    std::string kind = root.isMember("kind") ? root["kind"].asString() : "";
    CuRect region;
    if (root.isMember("region") && root["region"].isObject()) {
        auto& r = root["region"];
        region.x = r.isMember("x") ? r["x"].asDouble() : 0;
        region.y = r.isMember("y") ? r["y"].asDouble() : 0;
        region.w = r.isMember("w") ? r["w"].asDouble() : 0;
        region.h = r.isMember("h") ? r["h"].asDouble() : 0;
    }

    CuRect hit;
    if (kind == "image" && root.isMember("image"))
        hit = JZComputerUse::matchImage(root["image"].asString(), region);
    else if (kind == "color" && root.isMember("color"))
        hit = JZComputerUse::matchColor(root["color"].asString(), region);
    else if (kind == "ocr" && root.isMember("text"))
        // 文字定位(本地 PP-OCR)。这是**退化兜底**: 没有可访问布局时才该走到这里,
        // 且没 initOcr / 没模型 / 没识别到一律返回"未找到"(不假装成功)。
        hit = JZComputerUse::matchText(root["text"].asString(), region);
    else
        // yolo 需要检测模型后端, 当前未实现 -> 视为未找到。
        // ⚠ `layout` 也走不到这里: 布局路径由 **JS 侧**按 layoutTree() 的索引解析(layout.ts),
        //   不把路径规则复制到 C++(两边各写一套必然漂移)。见 docs/compute-use-design.md §4.1。
        return Json::Value();

    if (hit.w <= 0 || hit.h <= 0) return Json::Value();
    Json::Value out;
    out["x"] = hit.x; out["y"] = hit.y; out["w"] = hit.w; out["h"] = hit.h;
    return out;
}

// ————————————————————————————————————————————————
// 布局通道（运行期寻址的正路）：导出布局树 / 等界面稳定 / 启动激活窗口
// ————————————————————————————————————————————————
Json::Value JZJSComputerUseModule::jsLayoutTree(const std::string& optsJson) {
    // opts: { title?: string } —— 指定目标窗口(标题片段); 不给则用前台窗口
    std::string title;
    Json::Value root; Json::Reader reader;
    if (!optsJson.empty() && reader.parse(optsJson, root) && root.isMember("title"))
        title = root["title"].asString();
    // 找不到目标窗口时返回 Json::nullValue ⇒ JS 侧看作"没有布局"(节点会标 degraded)
    return JZComputerUse::layoutTree(title);
}

bool JZJSComputerUseModule::jsWaitStable(const std::string& optsJson) {
    int settle = 300, timeout = 10000;
    Json::Value root; Json::Reader reader;
    if (!optsJson.empty() && reader.parse(optsJson, root)) {
        if (root.isMember("settle"))    settle  = root["settle"].asInt();
        if (root.isMember("timeoutMs")) timeout = root["timeoutMs"].asInt();
    }
    return JZComputerUse::waitStable(settle, timeout);
}

bool JZJSComputerUseModule::jsLaunch(const std::string& optsJson) {
    Json::Value root; Json::Reader reader;
    if (optsJson.empty() || !reader.parse(optsJson, root)) return false;
    std::string exe   = root.isMember("exe")   ? root["exe"].asString()   : "";
    std::string title = root.isMember("title") ? root["title"].asString() : "";
    int timeout = root.isMember("timeoutMs") ? root["timeoutMs"].asInt() : 8000;
    std::string args;
    if (root.isMember("args") && root["args"].isArray()) {
        for (const auto& a : root["args"]) {
            if (!args.empty()) args += " ";
            args += a.asString();
        }
    }
    return JZComputerUse::launch(exe, args, title, timeout);
}

// ————————————————————————————————————————————————
// 进程 / 窗口管理（薄封装；语义与判据都在 JZComputerUse 那一层）
// ————————————————————————————————————————————————
bool JZJSComputerUseModule::jsFocusWindow(const std::string& title) {
    return JZComputerUse::focusWindow(title);
}
bool JZJSComputerUseModule::jsFocusProcess(const std::string& name) {
    return JZComputerUse::focusProcess(name);
}
bool JZJSComputerUseModule::jsTerminateProcess(const std::string& name) {
    return JZComputerUse::terminateProcess(name);
}
Json::Value JZJSComputerUseModule::jsWindows(const std::string& optsJson) {
    return JZComputerUse::windows(optsJson);
}
Json::Value JZJSComputerUseModule::jsWindowState(const std::string& optsJson) {
    return JZComputerUse::windowState(optsJson);
}

// ————————————————————————————————————————————————
// OCR 兜底（像素族里的"文字"；match({kind:'ocr',text}) 的后端）
// ————————————————————————————————————————————————
bool JZJSComputerUseModule::jsInitOcr(const std::string& optsJson) {
    // opts: { modelRoot?: string } —— 不给则按 C++ 侧的默认顺序(环境变量 / exe 旁 model/paddle_ocr)
    std::string modelRoot;
    Json::Value root; Json::Reader reader;
    if (!optsJson.empty() && reader.parse(optsJson, root) && root.isMember("modelRoot"))
        modelRoot = root["modelRoot"].asString();
    return JZComputerUse::initOcr(modelRoot);
}

bool JZJSComputerUseModule::jsOcrReady() {
    return JZComputerUse::ocrReady();
}

Json::Value JZJSComputerUseModule::jsOcrTexts(const std::string& optsJson) {
    CuRect region; double minScore = 0.6;
    Json::Value root; Json::Reader reader;
    if (!optsJson.empty() && reader.parse(optsJson, root)) {
        if (root.isMember("region") && root["region"].isObject()) {
            auto& r = root["region"];
            region.x = r.isMember("x") ? r["x"].asDouble() : 0;
            region.y = r.isMember("y") ? r["y"].asDouble() : 0;
            region.w = r.isMember("w") ? r["w"].asDouble() : 0;
            region.h = r.isMember("h") ? r["h"].asDouble() : 0;
        }
        if (root.isMember("minScore")) minScore = root["minScore"].asDouble();
    }
    return JZComputerUse::ocrTexts(region, minScore);
}

bool JZJSComputerUseModule::jsOcrBuilt() { return JZComputerUse::ocrBuilt(); }
Json::Value JZJSComputerUseModule::jsOcrInfo() { return JZComputerUse::ocrInfo(); }
void JZJSComputerUseModule::jsDeinitOcr() { JZComputerUse::deinitOcr(); }
