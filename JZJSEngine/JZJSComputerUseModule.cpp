#include "JZJSComputerUseModule.h"
#include "JZComputerUse.h"
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
    m_engine->registFunction(ns, "match",          jsbind::createSelfFunction(this, &JZJSComputerUseModule::jsMatch));
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
    else
        // ocr / yolo 需要 OCR / 检测模型后端, 当前未实现 -> 视为未找到
        return Json::Value();

    if (hit.w <= 0 || hit.h <= 0) return Json::Value();
    Json::Value out;
    out["x"] = hit.x; out["y"] = hit.y; out["w"] = hit.w; out["h"] = hit.h;
    return out;
}
