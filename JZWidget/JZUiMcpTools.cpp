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
#include <vector>
#include "json/json.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

JZ_OBJECT_IMPL(JZUiGetStateTool, JZMcpTool)
JZ_OBJECT_IMPL(JZUiScreenshotTool, JZMcpTool)
JZ_OBJECT_IMPL(JZUiWindowsTool, JZMcpTool)
JZ_OBJECT_IMPL(JZUiActivateWindowTool, JZMcpTool)
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

// 窗口位置尺寸: 屏幕逻辑坐标 [x, y, w, h]
static Json::Value windowBounds(JZWindow* w)
{
    JZPoint sp = w->mapToGlobal(JZPoint(0, 0));
    Json::Value b(Json::arrayValue);
    b.append(sp.x());
    b.append(sp.y());
    b.append(w->width());
    b.append(w->height());
    return b;
}

// 顶层窗口清单的简短文本 (错误提示用: 让 AI 能自己修正 target)
static std::string windowListText()
{
    std::string out;
    int i = 0;
    for (JZWindow* w : jzApp->topLevelWindows())
    {
        if (!w)
            continue;
        if (!out.empty())
            out += "; ";
        out += "#" + std::to_string(i) + " viewId=\"" + w->objectName() + "\""
             + " title=\"" + w->title() + "\""
             + (w->isVisible() ? " visible" : " hidden");
        ++i;
    }
    if (out.empty())
        out = "(当前没有顶层窗口)";
    return out;
}

// ==================== get_ui_state ====================

JZUiGetStateTool::JZUiGetStateTool(JZUiMcpBridge* bridge, JZObject* parent)
    : JZMcpTool(parent)
    , m_bridge(bridge)
{
    setName("get_ui_state");
    setDescription("导出**当前目标窗口**的无障碍语义树 (AccessibilityNodeInfo 风格 JSON: "
                   "className/viewId/text/contentDesc/boundsInScreen/能力位/children)。"
                   "目标窗口用 ui_windows 查看、ui_activate_window 切换。可选 maxDepth 限制深度。");
    setInputSchema(makeSchema({{"maxDepth", "number"}}, {}));
}

void JZUiGetStateTool::run(const Json::Value& id, const Json::Value& args)
{
    // MCP 请求来自 HTTP/WS 线程: 控件树操作必须投递到 UI 线程执行
    int maxDepth = args.get("maxDepth", -1).asInt();
    runOnThread(jzApp->mainThread(), [this, id, maxDepth]() {
        // 目标窗口每次现取: 期间可能被 ui_activate_window 换掉 / 被 JS 关掉
        JZWindow* root = m_bridge ? m_bridge->liveRoot() : nullptr;
        if (!root) {
            sigFinished.emit(id, makeTextResult("没有可用的目标窗口 (尚未创建或已关闭): "
                                                "先 ui_windows 查看, 再 ui_activate_window 切换", true));
            return;
        }
        Json::Value tree = root->dumpSemanticTree(maxDepth);
        sigFinished.emit(id, makeTextResult(tree.toStyledString()));
    });
}

// ==================== ui_screenshot ====================

JZUiScreenshotTool::JZUiScreenshotTool(JZUiMcpBridge* bridge, JZObject* parent)
    : JZMcpTool(parent)
    , m_bridge(bridge)
{
    setName("ui_screenshot");
    setDescription("抓取**当前目标窗口**当前帧保存为 PNG, 返回文件路径 (exe 目录下)。"
                   "目标窗口用 ui_windows 查看、ui_activate_window 切换。");
    setInputSchema(makeSchema({}, {}));
}

void JZUiScreenshotTool::run(const Json::Value& id, const Json::Value& args)
{
    (void)args;
    // GDI+/窗口操作线程亲和: 投递到 UI 线程执行
    runOnThread(jzApp->mainThread(), [this, id]() {
        JZWindow* root = m_bridge ? m_bridge->liveRoot() : nullptr;
        if (!root) {
            sigFinished.emit(id, makeTextResult("没有可用的目标窗口 (尚未创建或已关闭)", true));
            return;
        }
        screenshot(id, root);
    });
}

void JZUiScreenshotTool::screenshot(const Json::Value& id, JZWindow* root)
{
    // 抓图统一走 JZWidget::grab (PrintWindow + DPI 缩放 -> RGBA), 编码保存交给 JZImage
    JZImage img = root->grab();
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

// ==================== ui_windows / ui_activate_window ====================

JZUiWindowsTool::JZUiWindowsTool(JZUiMcpBridge* bridge, JZObject* parent)
    : JZMcpTool(parent)
    , m_bridge(bridge)
{
    setName("ui_windows");
    setDescription("列出所有顶层窗口: index / viewId(objectName) / title / className / visible / "
                   "native(原生窗口是否已创建) / active(系统当前活跃窗口) / current(本服务当前目标窗口) / "
                   "bounds[x,y,w,h](屏幕逻辑坐标)。配合 ui_activate_window 切换目标窗口。");
    setInputSchema(makeSchema({}, {}));
}

void JZUiWindowsTool::run(const Json::Value& id, const Json::Value& args)
{
    (void)args;
    runOnThread(jzApp->mainThread(), [this, id]() {
        JZWindow* active = jzApp->activeWindow();                   // 系统活跃窗口(可能为空)
        JZWindow* cur = m_bridge ? m_bridge->liveRoot() : nullptr;  // 本服务当前目标窗口

        std::vector<JZWindow*> list = jzApp->topLevelWindows();      // 拷贝: 遍历期间窗口表可能变
        Json::Value arr(Json::arrayValue);
        for (int i = 0; i < (int)list.size(); i++)
        {
            JZWindow* w = list[i];
            if (!w)
                continue;
            Json::Value e(Json::objectValue);
            e["index"] = i;
            if (!w->objectName().empty())
                e["viewId"] = w->objectName();
            e["title"] = w->title();
            e["className"] = w->aiRole();
            e["visible"] = w->isVisible();
            e["native"] = (w->nativeHandle() != nullptr);
            e["active"] = (w == active);
            e["current"] = (w == cur);
            e["bounds"] = windowBounds(w);
            arr.append(e);
        }

        Json::Value out(Json::objectValue);
        out["count"] = (int)arr.size();
        out["windows"] = arr;
        // 顶层字段直接给"活跃/当前"的名字, 少解析一层 (无名则用标题)
        out["active"] = active ? (active->objectName().empty() ? active->title() : active->objectName())
                               : std::string();
        out["current"] = cur ? (cur->objectName().empty() ? cur->title() : cur->objectName())
                             : std::string();
        sigFinished.emit(id, makeTextResult(Json::writeString(Json::StreamWriterBuilder(), out)));
    });
}

JZUiActivateWindowTool::JZUiActivateWindowTool(JZUiMcpBridge* bridge, JZObject* parent)
    : JZMcpTool(parent)
    , m_bridge(bridge)
{
    setName("ui_activate_window");
    setDescription("切换本服务的**当前目标窗口**并激活它(前置 + 焦点): 之后 get_ui_state / ui_screenshot / "
                   "ui_invoke 都作用于该窗口。target: '#viewId'(对象名) / 数字(ui_windows 里的 index, 0 基) / "
                   "标题子串(不区分大小写)。返回里 active 表示这次是否真的抢到前台 —— "
                   "Win32 前台锁定可能拒绝切换(目标窗口已切成功, 只是没抢到焦点), 这种情况再调一次即可。");
    setInputSchema(makeSchema({{"target", "string"}}, {"target"}));
}

void JZUiActivateWindowTool::run(const Json::Value& id, const Json::Value& args)
{
    std::string target = args.get("target", "").asString();
    runOnThread(jzApp->mainThread(), [this, id, target]() {
        if (!m_bridge) {
            sigFinished.emit(id, makeTextResult("UI MCP bridge 已失效", true));
            return;
        }

        JZWindow* w = m_bridge->findWindow(target);
        if (!w) {
            sigFinished.emit(id, makeTextResult("未找到顶层窗口: \"" + target +
                                                "\"; 可选: " + windowListText(), true));
            return;
        }

        m_bridge->setRoot(w);   // 切目标 + 激活 + 迁移 AI 覆盖层

        Json::Value out(Json::objectValue);
        out["ok"] = true;
        out["index"] = -1;
        std::vector<JZWindow*> list = jzApp->topLevelWindows();
        for (int i = 0; i < (int)list.size(); i++)
        {
            if (list[i] == w) {
                out["index"] = i;
                break;
            }
        }
        if (!w->objectName().empty())
            out["viewId"] = w->objectName();
        out["title"] = w->title();
        out["visible"] = w->isVisible();
        // active=false 说明系统前台锁定拒绝了这次切换 (Win32 规则), 目标窗口仍是本服务的 current
        out["active"] = w->isActiveWindow();
        out["bounds"] = windowBounds(w);
        sigFinished.emit(id, makeTextResult(Json::writeString(Json::StreamWriterBuilder(), out)));
    });
}

// ==================== ui_invoke ====================

JZUiInvokeTool::JZUiInvokeTool(JZUiMcpBridge* bridge, JZObject* parent)
    : JZMcpTool(parent)
    , m_bridge(bridge)
{
    setName("ui_invoke");
    setDescription("驱动**当前目标窗口**里的控件交互 (AI 鼠标可见移动)。target: '#viewId' 按对象名, "
                   "或 className(role) 如 'button'/'slider'; "
                   "action: click|toggle|setValue|setText|select; value: setValue/setText/select 的参数。"
                   "异步: AI 光标移动到目标后执行, 可用状态条上的取消按钮中断。"
                   "目标窗口用 ui_windows 查看、ui_activate_window 切换。");
    setInputSchema(makeSchema({{"target", "string"}, {"action", "string"}, {"value", "string"}},
                              {"target", "action"}));
    if (m_bridge && m_bridge->aiDriver())
        connect(&m_bridge->aiDriver()->sigDone, this, &JZUiInvokeTool::onAiDone);
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
    if (m_bridge && m_bridge->aiDriver())
        m_bridge->aiDriver()->cancel();
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
    // 目标窗口与驱动每次现取: 期间可能被 ui_activate_window 换掉 / 被 JS 关掉
    JZWindow* root = m_bridge ? m_bridge->liveRoot() : nullptr;
    JZAiDriver* driver = m_bridge ? m_bridge->aiDriver() : nullptr;
    if (!root || !driver) {
        sigFinished.emit(id, makeTextResult("没有可用的目标窗口 (尚未创建或已关闭): "
                                            "先 ui_windows 查看, 再 ui_activate_window 切换", true));
        return;
    }
    if (driver->busy()) {
        sigFinished.emit(id, makeTextResult("AI 操作进行中, 请等待或先取消", true));
        return;
    }
    JZWidget* w = resolveWidget(root, target);
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

    m_pendingSeq = driver->moveToWidget(w, needClick, status, std::move(onArrive));
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
    // AI 驱动(覆盖层/光标/合成点击)也随目标窗口走: setRoot 会把它迁到新窗口
    m_driver = new JZAiDriver(root, this);

    core()->setServerInfo("JZUi", "0.1.0");
    // 先"看局面"再"动控件": 窗口类工具排在最前
    core()->registerTool(new JZUiWindowsTool(this, core()));
    core()->registerTool(new JZUiActivateWindowTool(this, core()));
    core()->registerTool(new JZUiGetStateTool(this, core()));
    core()->registerTool(new JZUiScreenshotTool(this, core()));
    core()->registerTool(new JZUiInvokeTool(this, core()));
}

JZWindow* JZUiMcpBridge::liveRoot() const
{
    if (!m_root || !jzApp)
        return nullptr;

    // 目标窗口可能已被 JS 关窗销毁: 只在顶层窗口表里比指针, 绝不访问对象本身
    for (JZWindow* w : jzApp->topLevelWindows())
    {
        if (w == m_root)
            return m_root;
    }
    return nullptr;
}

void JZUiMcpBridge::setRoot(JZWindow* root)
{
    if (root == m_root)
        return;

    // 覆盖层随目标窗口迁移 (进行中的序列由 driver 作废并回报取消)
    m_driver->setRoot(root);
    m_root = root;

    // 切目标 = 同时把它带到前台 (系统前台锁定可能拒绝, 见 JZWindow::activateWindow)
    if (m_root)
        m_root->activateWindow();
}

JZWindow* JZUiMcpBridge::findWindow(const std::string& target) const
{
    if (target.empty() || !jzApp)
        return nullptr;

    std::vector<JZWindow*> list = jzApp->topLevelWindows();

    // 1) '#viewId' —— 按对象名精确匹配
    if (target[0] == '#')
    {
        std::string name = target.substr(1);
        for (JZWindow* w : list)
        {
            if (w && w->objectName() == name)
                return w;
        }
        return nullptr;
    }

    // 2) 纯数字 —— 按 ui_windows 的 index (0 基)
    if (target.find_first_not_of("0123456789") == std::string::npos)
    {
        int idx = std::stoi(target);
        if (idx >= 0 && idx < (int)list.size())
            return list[idx];
        return nullptr;
    }

    // 3) 其余 —— 标题子串 (不区分大小写); 可见者优先
    std::string want = lower(target);
    for (JZWindow* w : list)
    {
        if (w && w->isVisible() && lower(w->title()).find(want) != std::string::npos)
            return w;
    }
    for (JZWindow* w : list)
    {
        if (w && lower(w->title()).find(want) != std::string::npos)
            return w;
    }
    return nullptr;
}
