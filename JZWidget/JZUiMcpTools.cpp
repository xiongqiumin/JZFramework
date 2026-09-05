#include "JZUiMcpTools.h"
#include "JZMcpCore.h"
#include "JZWindow.h"
#include "JZAiDriver.h"
#include "JZPushButton.h"
#include "JZSlider.h"
#include "JZSwitch.h"
#include "JZCheckbox.h"
#include "JZRadioButton.h"
#include "JZComboBox.h"
#include "JZListWidget.h"
#include "JZLineEdit.h"
#include "JZTextEdit.h"
#include "JZScrollBar.h"
#include "JZMouseEvent.h"
#include "JZApplication.h"
#include "JZThread.h"
#include "JZImage.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

JZ_OBJECT_IMPL(JZUiGetStateTool, JZMcpTool)
JZ_OBJECT_IMPL(JZUiScreenshotTool, JZMcpTool)
JZ_OBJECT_IMPL(JZUiInvokeTool, JZMcpTool)
JZ_OBJECT_IMPL(JZUiMcpBridge, JZMcpServer)

// ==================== 公共小工具 ====================

static Json::Value makeTextResult(const std::string& text, bool isError = false)
{
    Json::Value content(Json::arrayValue);
    Json::Value item(Json::objectValue);
    item["type"] = "text";
    item["text"] = text;
    content.append(item);
    Json::Value result(Json::objectValue);
    result["content"] = content;
    result["isError"] = isError;
    return result;
}

static Json::Value makeSchema(
    std::initializer_list<std::pair<const char*, const char*>> props,
    std::initializer_list<const char*> required)
{
    Json::Value schema(Json::objectValue);
    schema["type"] = "object";
    Json::Value p(Json::objectValue);
    for (const auto& kv : props) {
        Json::Value t(Json::objectValue);
        t["type"] = kv.second;
        p[kv.first] = t;
    }
    schema["properties"] = p;
    Json::Value req(Json::arrayValue);
    for (const char* r : required)
        req.append(r);
    schema["required"] = req;
    return schema;
}

static std::string lower(std::string s)
{
    for (auto& c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

// 目标解析: "#viewId" 按对象名; 其余按 className(role) 匹配。
// 深度优先, 取第一个可见命中 (根节点自身也参与匹配)。
static JZWidget* resolveWidget(JZWidget* w, const std::string& target)
{
    if (target.empty())
        return nullptr;
    if (target[0] == '#') {
        if (w->objectName() == target.substr(1))
            return w;
    } else if (lower(w->aiRole()) == lower(target)) {
        return w;
    }

    for (auto* obj : w->children()) {
        auto* c = jzobject_cast<JZWidget*>(obj);
        if (!c || jzobject_cast<JZWindow*>(c) || !c->isVisible())
            continue;
        if (JZWidget* hit = resolveWidget(c, target))
            return hit;
    }
    return nullptr;
}

static std::string wideToUtf8(const std::wstring& ws)
{
    if (ws.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(),
                        &out[0], n, nullptr, nullptr);
    return out;
}

// ==================== get_ui_state ====================

JZUiGetStateTool::JZUiGetStateTool(JZWindow* root, JZObject* parent)
    : JZMcpTool(parent)
    , m_root(root)
{
    setName("get_ui_state");
    setDescription("导出目标窗口的无障碍语义树 (AccessibilityNodeInfo 风格 JSON: "
                   "className/viewId/text/contentDesc/boundsInScreen/能力位/children)。"
                   "可选 maxDepth 限制深度。");
    setInputSchema(makeSchema({{"maxDepth", "number"}}, {}));
}

void JZUiGetStateTool::run(const Json::Value& id, const Json::Value& args)
{
    // MCP 请求来自 HTTP/WS 线程: 控件树操作必须投递到 UI 线程执行
    int maxDepth = args.get("maxDepth", -1).asInt();
    runOnThread(jzApp->mainThread(), [this, id, maxDepth]() {
        Json::Value tree = m_root->dumpSemanticTree(maxDepth);
        sigFinished.emit(id, makeTextResult(tree.toStyledString()));
    });
}

// ==================== ui_screenshot ====================

JZUiScreenshotTool::JZUiScreenshotTool(JZWindow* root, JZObject* parent)
    : JZMcpTool(parent)
    , m_root(root)
{
    setName("ui_screenshot");
    setDescription("抓取目标窗口当前帧保存为 PNG, 返回文件路径 (exe 目录下)。");
    setInputSchema(makeSchema({}, {}));
}

void JZUiScreenshotTool::run(const Json::Value& id, const Json::Value& args)
{
    (void)args;
    // GDI+/窗口操作线程亲和: 投递到 UI 线程执行
    runOnThread(jzApp->mainThread(), [this, id]() {
        screenshot(id);
    });
}

void JZUiScreenshotTool::screenshot(const Json::Value& id)
{
    // 抓图统一走 JZWidget::grab (PrintWindow + DPI 缩放 -> RGBA), 编码保存交给 JZImage
    JZImage img = m_root->grab();
    if (img.isNull()) {
        sigFinished.emit(id, makeTextResult("截屏失败 (窗口未创建或客户区为空)", true));
        return;
    }

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L'\\') + 1);

    static int seq = 0;
    wchar_t name[64];
    swprintf(name, 64, L"ui_shot_%03d.png", ++seq);
    std::wstring full = dir + name;

    if (!img.save(wideToUtf8(full))) {
        sigFinished.emit(id, makeTextResult("PNG 保存失败", true));
        return;
    }
    sigFinished.emit(id, makeTextResult("screenshot saved: " + wideToUtf8(full) +
                                        " (" + std::to_string(img.width()) + "x" +
                                        std::to_string(img.height()) + " px)"));
}

// ==================== ui_invoke ====================

JZUiInvokeTool::JZUiInvokeTool(JZWindow* root, JZAiDriver* driver, JZObject* parent)
    : JZMcpTool(parent)
    , m_root(root)
    , m_driver(driver)
{
    setName("ui_invoke");
    setDescription("驱动控件交互 (AI 鼠标可见移动)。target: '#viewId' 按对象名, 或 className(role) 如 'button'/'slider'; "
                   "action: click|toggle|setValue|setText|select; value: setValue/setText/select 的参数。"
                   "异步: AI 光标移动到目标后执行, 可用状态条上的取消按钮中断。");
    setInputSchema(makeSchema({{"target", "string"}, {"action", "string"}, {"value", "string"}},
                              {"target", "action"}));
    connect(&m_driver->sigDone, this, &JZUiInvokeTool::onAiDone);
}

void JZUiInvokeTool::onAiDone(int seq, bool ok, const std::string& msg)
{
    if (m_pendingSeq != seq || m_pendingId.isNull())
        return;
    sigFinished.emit(m_pendingId, makeTextResult(msg, !ok));
    m_pendingId = Json::Value::null;
    m_pendingSeq = -1;
}

void JZUiInvokeTool::cancel(const Json::Value& id)
{
    (void)id;
    // 用户/MCP 取消: 中断驱动序列, 最终结果经 onAiDone 正常回送
    if (m_driver)
        m_driver->cancel();
}

void JZUiInvokeTool::run(const Json::Value& id, const Json::Value& args)
{
    // 解析/驱动全部在 UI 线程执行; 序列完成/取消后经 onAiDone 回送
    Json::Value capturedArgs = args;
    runOnThread(jzApp->mainThread(), [this, id, capturedArgs]() {
        invokeOnUi(id, capturedArgs);
    });
}

void JZUiInvokeTool::invokeOnUi(const Json::Value& id, const Json::Value& args)
{
    std::string target = args.get("target", "").asString();
    std::string action = args.get("action", "").asString();
    const Json::Value& value = args.get("value", Json::Value());

    if (target.empty() || action.empty()) {
        sigFinished.emit(id, makeTextResult("target 与 action 必填", true));
        return;
    }
    if (m_driver->busy()) {
        sigFinished.emit(id, makeTextResult("AI 操作进行中, 请等待或先取消", true));
        return;
    }
    JZWidget* w = resolveWidget(m_root, target);
    if (!w) {
        sigFinished.emit(id, makeTextResult("未找到目标: " + target, true));
        return;
    }

    // 动作分派: needClick = 到达后合成真实点击; 其它在到达回调里直调
    std::string verb;
    bool needClick = false;
    std::function<void()> onArrive;

    if (action == "click" || action == "toggle") {
        verb = "点击";
        needClick = true;   // click 语义覆盖 button/checkbox/switch/radiobutton/tab
    } else if (action == "setValue" || action == "setText") {
        verb = "设置";
        if (auto* s = jzobject_cast<JZSlider*>(w)) {
            if (!value.isNumeric()) {
                sigFinished.emit(id, makeTextResult("setValue 需要 number 参数", true));
                return;
            }
            int v = value.asInt();
            onArrive = [s, v]() {
                s->setValue(v);
                s->sigValueChanged.emit(s->value());
            };
        } else if (auto* sb = jzobject_cast<JZScrollBar*>(w)) {
            if (!value.isNumeric()) {
                sigFinished.emit(id, makeTextResult("setValue 需要 number 参数", true));
                return;
            }
            int v = value.asInt();
            onArrive = [sb, v]() { sb->setValue(v); };   // setValue 内部已 emit sigValueChanged
        } else if (auto* le = jzobject_cast<JZLineEdit*>(w)) {
            std::string v = value.asString();
            onArrive = [le, v]() { le->setText(v); };
        } else if (auto* te = jzobject_cast<JZTextEdit*>(w)) {
            std::string v = value.asString();
            onArrive = [te, v]() { te->setText(v); };
        } else {
            sigFinished.emit(id, makeTextResult(
                "目标 (" + w->aiRole() + ") 不支持 " + action, true));
            return;
        }
    } else if (action == "select") {
        verb = "选择";
        if (!value.isNumeric()) {
            sigFinished.emit(id, makeTextResult("select 需要 number 参数", true));
            return;
        }
        int idx = value.asInt();
        if (auto* cb = jzobject_cast<JZComboBox*>(w)) {
            onArrive = [cb, idx]() {
                cb->setCurrentIndex(idx);
                cb->update();
                cb->currentIndexChanged.emit(cb->currentIndex());
                cb->currentTextChanged.emit(cb->currentText());
            };
        } else if (auto* lw = jzobject_cast<JZListWidget*>(w)) {
            onArrive = [lw, idx]() { lw->setCurrentIndex(idx); };
        } else {
            sigFinished.emit(id, makeTextResult(
                "目标 (" + w->aiRole() + ") 不支持 select", true));
            return;
        }
    } else {
        sigFinished.emit(id, makeTextResult("未知动作: " + action, true));
        return;
    }

    // 状态条: "AI 正在点击: 'Update'" — 描述用可见文本, 无则用 role
    std::string desc = w->aiText();
    if (desc.empty())
        desc = w->aiRole();
    std::string status = "AI 正在" + verb + ": " + desc;

    m_pendingSeq = m_driver->moveToWidget(w, needClick, status, std::move(onArrive));
    if (m_pendingSeq < 0) {
        sigFinished.emit(id, makeTextResult("AI 操作进行中, 请等待或先取消", true));
        return;
    }
    m_pendingId = id;   // 序列已启动, 结果经 onAiDone 回送
}

// ==================== JZUiMcpBridge ====================

JZUiMcpBridge::JZUiMcpBridge(JZWindow* root, JZObject* parent)
    : JZMcpServer(parent)
    , m_root(root)
{
    m_driver = new JZAiDriver(root, this);
    core()->setServerInfo("JZUi", "0.1.0");
    core()->registerTool(new JZUiGetStateTool(m_root, core()));
    core()->registerTool(new JZUiScreenshotTool(m_root, core()));
    core()->registerTool(new JZUiInvokeTool(m_root, m_driver, core()));
}
