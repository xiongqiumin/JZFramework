// ==================== 虚拟桌面的 HTTP 接口 (把桌面当容器驱动) ====================
// 只做协议那一层 (路由 + JSON + 线程归属), 语义全部落在 JZVirtualDevice 上 —— 见头文件
// 里的五条纪律; 每条在下面都有对应的落地位置。
#include "JZVirtualDeviceHttp.h"
#include "JZVirtualDevice.h"
#include "JZVirtualWindow.h"
#include "JZVirtualThread.h"    // jzRunOnThreadSync (跨线程同步调用)
#include "JZHttpServer.h"
#include "JZApplication.h"      // jzApp->mainThread()
#include "JZThread.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZDebug.h"
#include "JZImage.h"

#include <map>
#include <memory>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace {

std::string jsonToText(const Json::Value& v)
{
    Json::StreamWriterBuilder b;
    b["indentation"] = "";      // 紧凑: driver 用不着好看
    b["emitUTF8"] = true;       // 中文不转义成 \uXXXX
    return Json::writeString(b, v);
}

bool textToJson(const std::string& text, Json::Value& out, std::string& error)
{
    if (text.empty()) {         // 空体: 有些端点没有参数 (当空对象)
        out = Json::Value(Json::objectValue);
        return true;
    }
    Json::CharReaderBuilder b;
    std::unique_ptr<Json::CharReader> reader(b.newCharReader());
    return reader->parse(text.data(), text.data() + text.size(), &out, &error);
}

Json::Value rectToJson(const JZRect& r)
{
    Json::Value a(Json::arrayValue);
    a.append(r.left());
    a.append(r.top());
    a.append(r.width());
    a.append(r.height());
    return a;
}

// ---- 路由参数 (路径里的值) 的百分号解码 ----
// ⚠ 框架只对 **query** 值做 URL 解码 (见 JZHttpRequest::setQuery), 路径参数是原样递进来的。
//   而"按标题找窗口 / 按名字找程序"这些用法里, 标题往往是中文, 客户端一定会 percent-encode
//   (`GET /api/windows/%E6%96%87%E6%9C%AC`) —— 这里不解一次, 中文标识在路径里就永远匹配不上,
//   而且很难看出为什么 (报错是"没有这个窗口: %E6%96%87%E6%9C%AC")。
//   非法序列 (孤零零的 %) 原样保留, 与 mongoose/mg_url_decode 的处理一致。
std::string percentDecode(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() &&
            isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            const char hex[3] = { s[i + 1], s[i + 2], '\0' };
            out += (char)strtol(hex, nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string bodyText(const JZHttpRequest& req)
{
    const JZByteArray& b = req.body();
    return std::string(b.data(), b.size());
}

// 名字 -> Win32 虚拟键码 (JZKeyEvent::key 就是 VK_*)。
// ⚠ 名字表与 JZComputerUse::key (真桌面那条路) **保持同一套** —— 两条路给同一批名字,
//   脚本才不用分叉。这里不能直接复用它: JZComputerUse 在 jz_virtual_process 那一层
//   (它要 Win32 SendInput), 而本层属于 jz_virtual_device (方向只能 process -> device)。
int vkFromName(const std::string& n)
{
    static const std::map<std::string, int> m = {
        {"Return", 0x0D}, {"Enter", 0x0D}, {"Tab", 0x09}, {"Escape", 0x1B}, {"Esc", 0x1B},
        {"Backspace", 0x08}, {"Delete", 0x2E}, {"Space", 0x20}, {"Insert", 0x2D},
        {"Home", 0x24}, {"End", 0x23}, {"PageUp", 0x21}, {"PageDown", 0x22},
        {"Up", 0x26}, {"Down", 0x28}, {"Left", 0x25}, {"Right", 0x27},
        {"LWin", 0x5B}, {"RWin", 0x5C}, {"Ctrl", 0x11}, {"Control", 0x11},
        {"Shift", 0x10}, {"Alt", 0x12}, {"Menu", 0x12},
        {"CapsLock", 0x14}, {"NumLock", 0x90}, {"ScrollLock", 0x91},
        {"F1", 0x70}, {"F2", 0x71}, {"F3", 0x72}, {"F4", 0x73}, {"F5", 0x74}, {"F6", 0x75},
        {"F7", 0x76}, {"F8", 0x77}, {"F9", 0x78}, {"F10", 0x79}, {"F11", 0x7A}, {"F12", 0x7B},
    };
    auto it = m.find(n);
    if (it != m.end())
        return it->second;
    // 单字符: 'a'~'z' / 'A'~'Z' / '0'~'9' (VK 就是大写字母/数字本身)
    if (n.size() == 1) {
        const char c = (char)::toupper((unsigned char)n[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            return (int)(unsigned char)c;
    }
    return 0;
}

JZMouseEvent::Button buttonFromName(const std::string& n)
{
    if (n == "right")  return JZMouseEvent::RightButton;
    if (n == "middle") return JZMouseEvent::MiddleButton;
    return JZMouseEvent::LeftButton;      // 缺省左键
}

// 修饰键: ["ctrl","shift","alt"] 数组, 或 "ctrl+shift" 串 (两种都收, 手敲脚本更顺)
bool modifiersFromJson(const Json::Value& v, uint32_t& out, std::string& error)
{
    out = JZKeyEvent::NoModifier;
    std::vector<std::string> names;
    if (v.isArray()) {
        for (const auto& item : v)
            names.push_back(item.asString());
    } else if (v.isString()) {
        std::string s = v.asString();
        size_t start = 0;
        while (start <= s.size()) {
            const size_t p = s.find('+', start);
            names.push_back(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
            if (p == std::string::npos)
                break;
            start = p + 1;
        }
    }
    for (std::string& name : names) {
        std::string k;
        for (char c : name)
            k += (char)::tolower((unsigned char)c);
        if (k.empty() || k == "none")
            continue;
        if (k == "ctrl" || k == "control")       out |= JZKeyEvent::CtrlModifier;
        else if (k == "shift")                   out |= JZKeyEvent::ShiftModifier;
        else if (k == "alt")                     out |= JZKeyEvent::AltModifier;
        else { error = "不认识的修饰键: " + name; return false; }
    }
    return true;
}

// 每个端点一个小 handler 对象 (JZHttpRoute 只存裸指针; 所有权在 JZVirtualDeviceHttp::m_handlers)
class JZFuncHttpHandler : public JZHttpHandler
{
public:
    explicit JZFuncHttpHandler(std::function<void(JZHttpServerClient*, const JZHttpRequest&)> fn)
        : m_fn(std::move(fn)) {}

    void handle(JZHttpServerClient* client, const JZHttpRequest& req) override
    {
        if (m_fn)
            m_fn(client, req);
    }

private:
    std::function<void(JZHttpServerClient*, const JZHttpRequest&)> m_fn;
};

// 请求体 -> JSON; 失败直接回 400 (每个 POST 端点开头都这一句)
bool takeBody(JZHttpServerClient* client, const JZHttpRequest& req, Json::Value& out)
{
    std::string error;
    if (!textToJson(bodyText(req), out, error)) {
        JZHttpRespone resp;
        Json::Value v(Json::objectValue);
        v["ok"] = false;
        v["error"] = "JSON 解析失败: " + error;
        resp.setJson(jsonToText(v));
        resp.setStatusCode(400);
        client->response(resp, req);
        return false;
    }
    return true;
}

} // namespace

JZVirtualDeviceHttp::JZVirtualDeviceHttp(JZVirtualDevice* screen, JZObject* parent)
    : JZObject(parent)
    , m_screen(screen)
{
    m_http = new JZHttpServer(this);

    addRoute("/api/health", [this](JZHttpServerClient* c, const JZHttpRequest& r) { handleHealth(c, r); });
    addRoute("/api/windows", [this](JZHttpServerClient* c, const JZHttpRequest& r) { handleWindows(c, r); });
    addRoute("/api/windows/<id>/activate",
             [this](JZHttpServerClient* c, const JZHttpRequest& r) { handleWindowActivate(c, r); });
    addRoute("/api/windows/<id>/state",
             [this](JZHttpServerClient* c, const JZHttpRequest& r) { handleWindowState(c, r); });
    addRoute("/api/windows/<id>/input",
             [this](JZHttpServerClient* c, const JZHttpRequest& r) { handleWindowInput(c, r); });
    addRoute("/api/windows/<id>",
             [this](JZHttpServerClient* c, const JZHttpRequest& r) { handleWindow(c, r); });
    addRoute("/api/screen", [this](JZHttpServerClient* c, const JZHttpRequest& r) { handleScreen(c, r); });
    addRoute("/api/desktop", [this](JZHttpServerClient* c, const JZHttpRequest& r) { handleDesktop(c, r); });
    addRoute("/api/input/mouse", [this](JZHttpServerClient* c, const JZHttpRequest& r) { handleMouse(c, r); });
    addRoute("/api/input/key", [this](JZHttpServerClient* c, const JZHttpRequest& r) { handleKey(c, r); });
    addRoute("/api/programs", [this](JZHttpServerClient* c, const JZHttpRequest& r) { handlePrograms(c, r); });
    addRoute("/api/programs/<name>/eval",
             [this](JZHttpServerClient* c, const JZHttpRequest& r) { handleProgramEval(c, r); });
    addRoute("/api/programs/<name>",
             [this](JZHttpServerClient* c, const JZHttpRequest& r) { handleProgramStop(c, r); });
}

JZVirtualDeviceHttp::~JZVirtualDeviceHttp()
{
    // 先停监听: 之后不会再有回调进来碰 m_screen / 半析构的自己
    if (m_http)
        m_http->close();
    // m_http 与各 handler 都是本对象的子对象 (见构造/addRoute), 由基类析构按序删除 ——
    // 顺序正合适: 路由表 (m_http 的成员) 先销毁, 指向 handler 的裸指针随之消失, 再删 handler。
}

void JZVirtualDeviceHttp::addRoute(const std::string& path, Handler fn)
{
    // handler 挂到 server 名下 (JZHttpRoute 只存裸指针, 得让它的生命盖住路由表)
    JZFuncHttpHandler* h = new JZFuncHttpHandler(fn);
    h->setParent(m_http);
    m_http->addRoute(path, h);
}

bool JZVirtualDeviceHttp::listen(int port, const std::string& ip)
{
    if (!m_screen || !m_http)
        return false;
    if (!m_http->listen(ip, port))
        return false;
    m_port = port;
    return true;
}

void JZVirtualDeviceHttp::close()
{
    if (m_http)
        m_http->close();
    m_port = 0;
}

bool JZVirtualDeviceHttp::isListening() const
{
    return m_http && m_http->isListening();
}

void JZVirtualDeviceHttp::setProgramHost(const JZVirtualProgramHost& host)
{
    m_host = host;
}

// ==================== 基础设施 ====================

bool JZVirtualDeviceHttp::onScreen(const std::function<void()>& fn)
{
    // 纪律 4: 桌面操作落在主线程 (合成/取帧/命中注入都只在这里安全)。
    // HTTP 回调本来就在这里 —— 下面这层只是"万一本对象建在别的线程"时的兜底。
    JZThread* main = jzApp ? jzApp->mainThread() : nullptr;
    if (!main || JZThread::currentThread() == main) {
        fn();
        return true;
    }
    return jzRunOnThreadSync(main, fn, 10000);
}

void JZVirtualDeviceHttp::replyJson(JZHttpServerClient* client, const JZHttpRequest& req,
                                    const Json::Value& value, int code)
{
    JZHttpRespone resp;
    resp.setStatusCode(code);
    resp.setJson(jsonToText(value));
    client->response(resp, req);
}

void JZVirtualDeviceHttp::replyError(JZHttpServerClient* client, const JZHttpRequest& req,
                                     int code, const std::string& message)
{
    Json::Value v(Json::objectValue);
    v["ok"] = false;
    v["error"] = message;
    replyJson(client, req, v, code);
}

// 窗口句柄 (与 HWND 同一个角色: 窗口生命期内唯一、稳定; 下标会随建窗/关窗变) ——
// 给的是本对象地址的十六进制, 对调用方是**不透明**的: 只用来"原样带回来", 不要去解析它。
std::string windowHandleOf(const JZVirtualWindow* w)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)(uintptr_t)w);
    return std::string(buf);
}

JZVirtualWindow* JZVirtualDeviceHttp::findWindow(const std::string& key) const
{
    if (!m_screen || key.empty())
        return nullptr;

    const std::string decoded = percentDecode(key);      // 路径里的值客户端会 percent-encode
    std::string k = (!decoded.empty() && decoded[0] == '#') ? decoded.substr(1) : decoded;
    if (k.empty())
        return nullptr;

    // "0x..." = 句柄 (最稳的写法: 窗口关掉之前一直是它)
    if (k.size() > 2 && k[0] == '0' && (k[1] == 'x' || k[1] == 'X')) {
        const uintptr_t want = (uintptr_t)strtoull(k.c_str() + 2, nullptr, 16);
        for (JZVirtualWindow* w : m_screen->windowList()) {
            if ((uintptr_t)w == want)
                return w;
        }
        return nullptr;
    }

    // "3" = z 序下标 (与 /api/windows 给的 index 同一口径)
    bool digits = true;
    for (char c : k)
        if (c < '0' || c > '9') { digits = false; break; }
    if (digits) {
        const int idx = atoi(k.c_str());
        const std::vector<JZVirtualWindow*> list = m_screen->windowList();
        if (idx >= 0 && idx < (int)list.size())
            return list[idx];
        return nullptr;
    }

    return m_screen->byTitle(k);      // 其余: 标题片段 (最上层那个)
}

Json::Value JZVirtualDeviceHttp::windowJson(JZVirtualWindow* w, int index) const
{
    Json::Value o(Json::objectValue);
    if (!w)
        return o;
    o["id"] = windowHandleOf(w);                     // 稳定标识 (见 findWindow)
    o["index"] = index;                              // 本次枚举里的 z 序下标 (会变)
    o["title"] = w->title();
    o["frame"] = rectToJson(w->frameGeometry());     // 外框 (含标题栏/边框), 桌面逻辑坐标
    o["client"] = rectToJson(w->clientGeometry());   // 客户区 (截 ?window= 与控件几何同口径)
    o["visible"] = w->isVisible();
    o["active"] = (w == m_screen->activeWindow());
    o["minimized"] = w->isMinimized();
    o["maximized"] = w->isMaximized();
    // 归属: 哪个程序建的 (进程名当对象名, 见 JZVirtualProcess 构造); 空 = 宿主自己建的
    JZThread* owner = w->process();
    o["process"] = owner ? owner->objectName() : std::string();
    return o;
}

// 某个程序建的窗口: 先按对象名在进程表里找到那个进程, 再问桌面 (windowsOfProcess) ——
// 桌面库不认识"程序名", 但认识"进程 -> 它的窗口" (JZVirtualWindow::process 就是那个 JZThread)。
Json::Value JZVirtualDeviceHttp::windowsOfProgram(const std::string& name) const
{
    Json::Value arr(Json::arrayValue);
    if (!m_screen)
        return arr;

    JZThread* proc = nullptr;
    for (JZThread* t : m_screen->processes()) {
        if (t && t->objectName() == name) {
            proc = t;
            break;
        }
    }
    const std::vector<JZVirtualWindow*> list = m_screen->windowList();
    const std::vector<JZVirtualWindow*> mine = m_screen->windowsOfProcess(proc);
    for (JZVirtualWindow* w : mine) {
        // index 取"在整张窗口表里的位置" (与 /api/windows 同一口径, 便于交叉引用)
        int idx = -1;
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i] == w)
                idx = (int)i;
        arr.append(windowJson(w, idx));
    }
    return arr;
}

// 请求体 -> "加载什么": file = 一个 JS 程序 (按 ESM 模块加载), script = 一段代码 (直接跑)。
// 走模块那条才是"加载真程序": import 能用, 而且引擎会把入口模块登记成 appFile ⇒
// `jzApp.applicationDirPath()` = 程序所在目录 (程序按入口目录找随它走的资源, 这条约定才成立)。
bool JZVirtualDeviceHttp::resolveProgram(const Json::Value& body, std::string& script,
                                         std::string& workDir, std::string& moduleRel,
                                         std::string& error) const
{
    script.clear();
    workDir.clear();
    moduleRel.clear();

    if (body.isMember("file")) {
        const std::string path = body["file"].asString();
        if (path.empty() || !JZFileInfo(path).exists()) {
            error = "找不到程序文件: " + path;
            return false;
        }
        // 运行根 = 程序所在目录 (缺省); 模块名 = 文件名 (相对运行根, 扩展名可省)
        workDir = body.get("workDir", "").asString();
        const int slash = (int)path.find_last_of("/\\");
        const std::string dir = (slash > 0) ? path.substr(0, (size_t)slash) : std::string();
        if (workDir.empty())
            workDir = dir;
        moduleRel = (slash > 0) ? path.substr((size_t)slash + 1) : path;
        return true;
    }
    if (body.isMember("script")) {
        script = body["script"].asString();
        workDir = body.get("workDir", "").asString();
        if (script.empty()) {
            error = "script 是空的";
            return false;
        }
        return true;
    }
    error = "要给 file (一个 JS 程序) 或 script (一段代码)";
    return false;
}

bool JZVirtualDeviceHttp::readScript(const Json::Value& body, std::string& script,
                                     std::string& workDir, std::string& error) const
{
    if (body.isMember("script")) {
        script = body["script"].asString();
        workDir = body.get("workDir", "").asString();
        if (script.empty()) {
            error = "script 是空的";
            return false;
        }
        return true;
    }
    if (body.isMember("file")) {
        const std::string path = body["file"].asString();
        JZFile f;
        if (path.empty() || !f.open(path, "rb")) {
            error = "读不到脚本文件: " + path;
            return false;
        }
        script = str_fromUtf8(f.readAll());       // TS/JS 源码按 UTF-8 存
        f.close();
        if (script.empty()) {
            error = "脚本文件是空的: " + path;
            return false;
        }
        workDir = body.get("workDir", "").asString();
        if (workDir.empty()) {
            // 缺省运行根 = 脚本所在目录: 引擎按它找相对路径/模块 (与 code_runtime --entry 一致)
            const int p = (int)path.find_last_of("/\\");
            if (p > 0)
                workDir = path.substr(0, (size_t)p);
        }
        return true;
    }
    error = "要给 script (源码) 或 file (脚本路径)";
    return false;
}

// ==================== 状态 / 窗口 ====================

void JZVirtualDeviceHttp::handleHealth(JZHttpServerClient* client, const JZHttpRequest& req)
{
    auto box = std::make_shared<Json::Value>(Json::objectValue);
    const bool ok = onScreen([this, box]() {
        Json::Value d(Json::objectValue);
        d["w"] = m_screen->desktopWidth();
        d["h"] = m_screen->desktopHeight();
        d["scale"] = m_screen->scale();
        (*box)["ok"] = true;
        (*box)["desktop"] = d;
        (*box)["windows"] = (int)m_screen->windowList().size();
        JZVirtualWindow* a = m_screen->activeWindow();
        (*box)["active"] = a ? a->title() : std::string();
        (*box)["frame"] = (Json::UInt)m_screen->frameSerial();      // 合成序号: 变了 = 画面变了
        (*box)["programs"] = m_screen->processCount();
        (*box)["host"] = m_host.launch ? "ready" : "none";          // 程序接口能不能用
    });
    if (!ok) {
        replyError(client, req, 500, "桌面线程没有响应");
        return;
    }
    replyJson(client, req, *box);
}

void JZVirtualDeviceHttp::handleWindows(JZHttpServerClient* client, const JZHttpRequest& req)
{
    if (req.method() == JZHttpRequest::GET) {
        // 过滤参数: 程序名 / 标题片段 / 只可见 / WindowFromPoint。
        // 过滤**不改坐标也不改 z 序** —— index 仍是它在整张窗口表里的下标 (便于与别的接口对上),
        // 所以过滤后的列表里 index 可能不连续, 这是故意的。
        const std::string fProcess = req.queryParam("process");
        const std::string fTitle = req.queryParam("title");
        const std::string fVisible = req.queryParam("visible");
        const std::string fAt = req.queryParam("at");           // "x,y" = WindowFromPoint
        int atX = 0, atY = 0;
        bool hasAt = false;
        if (!fAt.empty()) {
            const size_t comma = fAt.find(',');
            if (comma == std::string::npos) {
                replyError(client, req, 400, "at 要写成 x,y");
                return;
            }
            atX = atoi(fAt.substr(0, comma).c_str());
            atY = atoi(fAt.substr(comma + 1).c_str());
            hasAt = true;
        }

        auto box = std::make_shared<Json::Value>(Json::objectValue);
        const bool ok = onScreen([this, box, fProcess, fTitle, fVisible, hasAt, atX, atY]() {
            const std::vector<JZVirtualWindow*> list = m_screen->windowList();
            JZVirtualWindow* active = m_screen->activeWindow();
            JZVirtualWindow* hit = hasAt ? m_screen->windowAt(atX, atY) : nullptr;
            bool visibleOnly = false;
            if (fVisible == "1" || fVisible == "true" || fVisible == "yes")
                visibleOnly = true;

            Json::Value arr(Json::arrayValue);
            int activeIdx = -1;
            for (size_t i = 0; i < list.size(); ++i) {
                JZVirtualWindow* w = list[i];
                if (hasAt && w != hit)
                    continue;                             // WindowFromPoint: 只要那一个
                if (visibleOnly && !w->isVisible())
                    continue;
                if (!fTitle.empty() && w->title().find(fTitle) == std::string::npos)
                    continue;
                if (!fProcess.empty()) {
                    JZThread* owner = w->process();
                    const std::string ownerName = owner ? owner->objectName() : std::string();
                    if (ownerName != fProcess)
                        continue;
                }
                arr.append(windowJson(w, (int)i));
                if (w == active)
                    activeIdx = (int)i;
            }
            (*box)["ok"] = true;
            (*box)["count"] = (int)arr.size();
            (*box)["active"] = activeIdx;
            (*box)["windows"] = arr;
            if (hasAt) {
                // 点在哪: 顺带把"那个点测到谁"和"它在客户区里的坐标"给出来 (省得调用方再算)
                (*box)["at"] = Json::Value(Json::arrayValue);
                (*box)["at"].append(atX);
                (*box)["at"].append(atY);
                if (hit) {
                    const JZPoint c = hit->screenToClient(JZPoint(atX, atY));
                    (*box)["clientAt"] = Json::Value(Json::arrayValue);
                    (*box)["clientAt"].append(c.x());
                    (*box)["clientAt"].append(c.y());
                }
            }
        });
        if (!ok) {
            replyError(client, req, 500, "桌面线程没有响应");
            return;
        }
        replyJson(client, req, *box);
        return;
    }

    if (req.method() != JZHttpRequest::POST) {
        replyError(client, req, 405, "只支持 GET (列表) 与 POST (新建)");
        return;
    }

    Json::Value body;
    if (!takeBody(client, req, body))
        return;

    const int w = body.get("w", 320).asInt();
    const int h = body.get("h", 240).asInt();
    const std::string title = body.get("title", "窗口").asString();
    const bool hasPos = body.isMember("x") && body.isMember("y");
    const int x = body.get("x", 0).asInt();
    const int y = body.get("y", 0).asInt();

    auto box = std::make_shared<Json::Value>(Json::objectValue);
    auto fail = std::make_shared<std::string>();
    const bool ok = onScreen([this, box, fail, w, h, title, hasPos, x, y]() {
        JZVirtualWindow* win = hasPos ? m_screen->createWindow(w, h, title, x, y)
                                      : m_screen->createWindow(w, h, title);
        if (!win) {
            *fail = "建窗口失败";
            return;
        }
        const std::vector<JZVirtualWindow*> list = m_screen->windowList();
        int idx = -1;
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i] == win)
                idx = (int)i;
        (*box)["ok"] = true;
        (*box)["window"] = windowJson(win, idx);
    });
    if (!ok) {
        replyError(client, req, 500, "桌面线程没有响应");
        return;
    }
    if (!fail->empty()) {
        replyError(client, req, 400, *fail);
        return;
    }
    replyJson(client, req, *box);
}

void JZVirtualDeviceHttp::handleWindowActivate(JZHttpServerClient* client, const JZHttpRequest& req)
{
    const std::string key = req.param("id");
    auto box = std::make_shared<Json::Value>(Json::objectValue);
    auto fail = std::make_shared<std::string>();
    const bool ok = onScreen([this, box, fail, key]() {
        JZVirtualWindow* w = findWindow(key);
        if (!w) {
            *fail = "没有这个窗口: " + key;
            return;
        }
        const std::vector<JZVirtualWindow*> list = m_screen->windowList();
        int idx = -1;
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i] == w)
                idx = (int)i;
        m_screen->activate(w);
        if (w->isMinimized())
            w->setMinimized(false);
        m_screen->refresh();
        (*box)["ok"] = true;
        (*box)["window"] = windowJson(w, idx);
    });
    if (!ok) {
        replyError(client, req, 500, "桌面线程没有响应");
        return;
    }
    if (!fail->empty()) {
        replyError(client, req, 404, *fail);
        return;
    }
    replyJson(client, req, *box);
}

// /api/windows/<id> : GET = 查这一个窗口, DELETE = 关掉它 (等价 /state {"action":"close"})
void JZVirtualDeviceHttp::handleWindow(JZHttpServerClient* client, const JZHttpRequest& req)
{
    if (req.method() != JZHttpRequest::GET && req.method() != JZHttpRequest::DELETE) {
        replyError(client, req, 405, "只支持 GET (查这一个) 与 DELETE (关掉)");
        return;
    }
    const bool closing = (req.method() == JZHttpRequest::DELETE);
    const std::string key = req.param("id");
    auto box = std::make_shared<Json::Value>(Json::objectValue);
    auto fail = std::make_shared<std::string>();
    const bool ok = onScreen([this, box, fail, key, closing]() {
        JZVirtualWindow* w = findWindow(key);
        if (!w) {
            *fail = "没有这个窗口: " + key;
            return;
        }
        if (closing) {
            (*box)["ok"] = true;
            (*box)["id"] = windowHandleOf(w);
            (*box)["title"] = w->title();
            m_screen->destroyWindow(w);       // ⚠ 之后 w 已失效
            return;
        }
        const std::vector<JZVirtualWindow*> list = m_screen->windowList();
        int idx = -1;
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i] == w)
                idx = (int)i;
        (*box)["ok"] = true;
        (*box)["window"] = windowJson(w, idx);
    });
    if (!ok) {
        replyError(client, req, 500, "桌面线程没有响应");
        return;
    }
    if (!fail->empty()) {
        replyError(client, req, 404, *fail);
        return;
    }
    replyJson(client, req, *box);
}

// /api/windows/<id>/state : SetWindowPos / ShowWindow / SetWindowText 那一组。
// 字段都可选, 给哪个改哪个; action 单独给也能用 (只改状态)。
void JZVirtualDeviceHttp::handleWindowState(JZHttpServerClient* client, const JZHttpRequest& req)
{
    if (req.method() != JZHttpRequest::POST) {
        replyError(client, req, 405, "只支持 POST");
        return;
    }
    Json::Value body;
    if (!takeBody(client, req, body))
        return;

    const std::string key = req.param("id");
    const std::string action = body.get("action", "").asString();
    const bool hasPos = body.isMember("x") && body.isMember("y");
    const bool hasSize = body.isMember("w") && body.isMember("h");
    const bool hasTitle = body.isMember("title");
    const int x = body.get("x", 0).asInt();
    const int y = body.get("y", 0).asInt();
    const int cw = body.get("w", 0).asInt();
    const int ch = body.get("h", 0).asInt();
    const std::string title = body.get("title", "").asString();
    if (action.empty() && !hasPos && !hasSize && !hasTitle) {
        replyError(client, req, 400,
                   "要给 action (activate|minimize|maximize|restore|close) 或 x/y、w/h (客户区尺寸)、title");
        return;
    }
    if (!action.empty() && action != "activate" && action != "minimize" && action != "maximize" &&
        action != "restore" && action != "close") {
        replyError(client, req, 400, "不认识的 action: " + action);
        return;
    }

    auto box = std::make_shared<Json::Value>(Json::objectValue);
    auto fail = std::make_shared<std::string>();
    auto notFound = std::make_shared<bool>(false);     // 找不到窗口 = 404, 参数不对 = 400
    const bool ok = onScreen([this, box, fail, notFound, key, action, hasPos, hasSize, hasTitle,
                              x, y, cw, ch, title]() {
        JZVirtualWindow* w = findWindow(key);
        if (!w) {
            *fail = "没有这个窗口: " + key;
            *notFound = true;
            return;
        }
        if (action == "close") {
            (*box)["ok"] = true;
            (*box)["id"] = windowHandleOf(w);
            (*box)["title"] = w->title();
            m_screen->destroyWindow(w);
            return;
        }
        if (action == "activate") {
            if (w->isMinimized())
                w->setMinimized(false);       // 激活一个最小化的窗口, 顺手还原 (窗口管理器同语义)
            m_screen->activate(w);
        } else if (action == "minimize") {
            w->setMinimized(true);
        } else if (action == "maximize") {
            if (w->isMinimized())
                w->setMinimized(false);
            w->setMaximized(true);
        } else if (action == "restore") {
            if (w->isMinimized())
                w->setMinimized(false);
            if (w->isMaximized())
                w->setMaximized(false);
        }
        // 位置 = 外框坐标 (与 frame[0..1] 同口径); 尺寸 = **客户区** (与 client[2..3] 同口径),
        // 装饰的换算是 JZVirtualWindow::setClientSize 的事 —— 脚本里不出现 kBorder/kTitleBar。
        if (hasSize) {
            if (cw <= 0 || ch <= 0) {
                *fail = "w/h 要 > 0 (它们 = 客户区尺寸)";
                return;
            }
            w->setClientSize(cw, ch);
        }
        if (hasPos)
            w->setFramePos(x, y);
        if (hasTitle)
            w->setTitle(title);
        m_screen->refresh();

        const std::vector<JZVirtualWindow*> list = m_screen->windowList();
        int idx = -1;
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i] == w)
                idx = (int)i;
        (*box)["ok"] = true;
        (*box)["window"] = windowJson(w, idx);
    });
    if (!ok) {
        replyError(client, req, 500, "桌面线程没有响应");
        return;
    }
    if (!fail->empty()) {
        replyError(client, req, *notFound ? 404 : 400, *fail);
        return;
    }
    replyJson(client, req, *box);
}

// ==================== 截图 ====================

void JZVirtualDeviceHttp::handleScreen(JZHttpServerClient* client, const JZHttpRequest& req)
{
    const std::string windowKey = req.queryParam("window");
    const std::string format = req.queryParam("format");
    const bool asJson = (format == "json" || format == "dataurl");

    auto img = std::make_shared<JZImage>();
    auto meta = std::make_shared<Json::Value>(Json::objectValue);
    auto fail = std::make_shared<std::string>();
    const bool ok = onScreen([this, img, meta, fail, windowKey]() {
        JZVirtualWindow* w = nullptr;
        if (!windowKey.empty()) {
            w = findWindow(windowKey);
            if (!w) {
                *fail = "没有这个窗口: " + windowKey;
                return;
            }
        }
        // 纪律 2: 给了 window 就给**客户区** (与控件几何/命中同一坐标系); 不给 = 整屏
        if (w) {
            *img = m_screen->screenshotWindow(w);
            (*meta)["logical"] = rectToJson(JZRect(0, 0, w->clientGeometry().width(),
                                                   w->clientGeometry().height()));
            (*meta)["window"] = w->title();
            (*meta)["frame"] = rectToJson(w->frameGeometry());
        } else {
            *img = m_screen->screenshot();
            (*meta)["logical"] = rectToJson(JZRect(0, 0, m_screen->desktopWidth(),
                                                   m_screen->desktopHeight()));
            (*meta)["window"] = std::string();
        }
        (*meta)["scale"] = m_screen->scale();
    });
    if (!ok) {
        replyError(client, req, 500, "桌面线程没有响应");
        return;
    }
    if (!fail->empty()) {
        replyError(client, req, 404, *fail);
        return;
    }
    if (img->isNull() || !img->data()) {
        replyError(client, req, 500, "这一帧取不到 (窗口/进程可能正在退出)");
        return;
    }

    const JZByteArray png = img->encode(JZImage::Format::Png);
    if (png.isEmpty()) {
        replyError(client, req, 500, "PNG 编码失败");
        return;
    }

    if (!asJson) {
        // 默认: 直接是 PNG 二进制 —— driver 落地成文件就能当"屏幕"
        JZHttpRespone resp;
        resp.setContentType("image/png");
        resp.setBody(png);
        client->response(resp, req);
        return;
    }

    const JZByteArray b64 = png.toBase64();
    Json::Value out = *meta;
    out["ok"] = true;
    out["png"] = Json::Value(Json::arrayValue);
    out["png"].append(img->width());
    out["png"].append(img->height());
    out["dataUrl"] = std::string("data:image/png;base64,") + std::string(b64.data(), b64.size());
    replyJson(client, req, out);
}

void JZVirtualDeviceHttp::handleDesktop(JZHttpServerClient* client, const JZHttpRequest& req)
{
    if (req.method() != JZHttpRequest::POST) {
        replyError(client, req, 405, "只支持 POST");
        return;
    }
    Json::Value body;
    if (!takeBody(client, req, body))
        return;
    if (!body.isMember("scale")) {
        replyError(client, req, 400, "要给 scale (例如 1 或 2)");
        return;
    }
    const double s = body["scale"].asDouble();
    if (!(s > 0.0)) {
        replyError(client, req, 400, "scale 必须 > 0");
        return;
    }
    auto box = std::make_shared<Json::Value>(Json::objectValue);
    const bool ok = onScreen([this, box, s]() {
        m_screen->setScale((float)s);          // 纪律 1: 只改截图分辨率, 坐标一个都不动
        (*box)["ok"] = true;
        (*box)["scale"] = m_screen->scale();
    });
    if (!ok) {
        replyError(client, req, 500, "桌面线程没有响应");
        return;
    }
    replyJson(client, req, *box);
}

// ==================== 输入 ====================
// 三件事分开看, 但代码只有一份:
//   ① 参数解析与"投给谁"无关 —— 桌面那条路与单窗口那条路共用同一套 action/x/y/...;
//   ② 差异只在**投递口**: 桌面 = 命中测试 + 激活 + 按命中区域分发 (标题栏拖动/边框缩放/
//      客户区投控件); 单窗口 = 直接投给它的控件树, **不激活它** (等价 SendMessage);
//   ③ 坐标: 桌面那条路恒为**桌面**逻辑坐标; 单窗口那条路默认**客户区**坐标 (与 PostMessage
//      的 lParam 同口径), 写 "coords":"screen" 就是桌面坐标, 内部换算成客户区再投。
// ⚠ 单窗口那条路只带"窗口标识"进来, 在 onScreen 里才 findWindow —— 窗口指针不能跨 handler
//   缓存, 它可能被它自己的线程关掉。
void JZVirtualDeviceHttp::doMouseInput(const Json::Value& body, JZVirtualWindow* target,
                                       Json::Value& out, std::string& error)
{
    const std::string action = body.get("action", "").asString();
    const int x = body.get("x", -1).asInt();
    const int y = body.get("y", -1).asInt();
    const JZMouseEvent::Button button = buttonFromName(body.get("button", "left").asString());
    const int steps = body.get("steps", 5).asInt();
    const int delta = body.get("delta", 1).asInt();
    uint32_t mods = JZKeyEvent::NoModifier;
    if (!modifiersFromJson(body.get("modifiers", Json::Value(Json::nullValue)), mods, error))
        return;
    if (action.empty()) {
        error = "要给 action (move/down/up/click/double/drag/wheel)";
        return;
    }
    if (action != "wheel" && (x < 0 || y < 0)) {
        error = target ? "要给客户区坐标 x/y (逻辑像素)" : "要给桌面坐标 x/y (逻辑像素)";
        return;
    }

    // 坐标统一成"投递口要的那一套": 单窗口 = 客户区; 桌面 = 桌面坐标 (不换算)
    const std::string coords = body.get("coords", "client").asString();
    const bool needConvert = target && coords != "client";
    auto conv = [target, needConvert](int px, int py) {
        if (!target || !needConvert)
            return JZPoint(px, py);
        return target->screenToClient(JZPoint(px, py));
    };
    const JZPoint p0 = conv(x, y);
    const JZPoint p1 = conv(body.get("toX", x).asInt(), body.get("toY", y).asInt());

    // 两个投递口收成一个: 桌面 (px/py = 桌面坐标) / 单窗口 (px/py = 客户区坐标)
    auto send = [&](JZMouseEvent::MouseType type, const JZPoint& p, int wheel) -> bool {
        if (target)
            return target->injectMouse(type, button, p.x(), p.y(), wheel, mods);
        return m_screen->injectMouseAtScreen(type, button, p.x(), p.y(), wheel, mods);
    };

    // 命中谁 (点击**之前**算, 便于 driver 判断"点到哪个窗口了"; 点到 chrome/窗口外就是空)
    JZVirtualWindow* hit = target ? target : m_screen->windowAt(x, y);

    bool handled = false;
    if (action == "move") {
        handled = send(JZMouseEvent::Move, p0, 0);
    } else if (action == "down") {
        handled = send(JZMouseEvent::Press, p0, 0);
    } else if (action == "up") {
        handled = send(JZMouseEvent::Release, p0, 0);
    } else if (action == "click") {
        handled = send(JZMouseEvent::Press, p0, 0);
        handled = send(JZMouseEvent::Release, p0, 0) || handled;
    } else if (action == "double" || action == "doubleclick") {
        for (int i = 0; i < 2; ++i) {
            send(JZMouseEvent::Press, p0, 0);
            send(JZMouseEvent::Release, p0, 0);
        }
        handled = true;
    } else if (action == "drag") {
        const int n = steps > 0 ? steps : 1;
        handled = send(JZMouseEvent::Press, p0, 0);
        for (int i = 1; i <= n; ++i) {
            const JZPoint ip(p0.x() + (p1.x() - p0.x()) * i / n,
                             p0.y() + (p1.y() - p0.y()) * i / n);
            send(JZMouseEvent::Move, ip, 0);
        }
        handled = send(JZMouseEvent::Release, p1, 0) || handled;
    } else if (action == "wheel") {
        handled = send(JZMouseEvent::Wheel, p0, delta);
    } else {
        error = "不认识的 action: " + action;
        return;
    }

    out["ok"] = true;
    out["action"] = action;
    out["x"] = x;
    out["y"] = y;
    out["coords"] = target ? (needConvert ? "screen" : "client") : "screen";
    if (target) {
        // 实际投进去的客户区坐标 (给了 screen 坐标时这里就是换算结果)
        out["client"] = Json::Value(Json::arrayValue);
        out["client"].append(p0.x());
        out["client"].append(p0.y());
    }
    out["handled"] = handled;                       // 有没有真的落进客户区的控件
    out["window"] = hit ? hit->title() : std::string();
}

void JZVirtualDeviceHttp::doKeyInput(const Json::Value& body, JZVirtualWindow* target,
                                     Json::Value& out, std::string& error)
{
    uint32_t mods = JZKeyEvent::NoModifier;
    if (!modifiersFromJson(body.get("modifiers", Json::Value(Json::nullValue)), mods, error))
        return;
    const std::string text = body.get("text", "").asString();
    const std::string keyName = body.get("key", "").asString();
    const bool press = body.get("press", true).asBool();
    if (text.empty() && keyName.empty()) {
        error = "要给 key (如 \"Return\"/\"Ctrl+C\" 里的主键) 或 text (整段文本)";
        return;
    }

    int vk = 0;
    if (!keyName.empty()) {
        // "Ctrl+C" 这种组合名: 拆出修饰键再注入 (与 JZComputerUse::key 的写法一致)
        std::string name = keyName;
        size_t start = 0;
        while (true) {
            const size_t plus = name.find('+', start);
            if (plus == std::string::npos)
                break;
            std::string mod = name.substr(start, plus - start);
            std::string lower;
            for (char c : mod)
                lower += (char)::tolower((unsigned char)c);
            if (lower == "ctrl" || lower == "control")   mods |= JZKeyEvent::CtrlModifier;
            else if (lower == "shift")                   mods |= JZKeyEvent::ShiftModifier;
            else if (lower == "alt")                     mods |= JZKeyEvent::AltModifier;
            start = plus + 1;
        }
        if (start > 0)
            name = name.substr(start);
        vk = vkFromName(name);
        if (vk == 0) {
            error = "不认识按键名: " + keyName;
            return;
        }
    }

    // 投递口: 单窗口 = 直接投它的控件树 (不管它是不是活动窗口); 桌面 = 投给**活动**窗口
    bool handled = false;
    if (vk != 0) {
        handled = target ? target->injectKey((uint32_t)vk, std::string(), mods, press)
                         : m_screen->injectKey((uint32_t)vk, std::string(), mods, press);
    }
    if (!text.empty()) {
        const bool typed = target ? target->injectText(text) : m_screen->injectText(text);
        handled = typed || handled;
    }

    JZVirtualWindow* win = target ? target : m_screen->activeWindow();
    out["ok"] = true;
    out["vk"] = vk;
    out["text"] = text;
    out["handled"] = handled;
    out["window"] = win ? win->title() : std::string();
}

// 鼠标/键盘的共同外壳: 校验方法 + 取体 + 决定"投给谁", 然后在主线程里解析并投递
void JZVirtualDeviceHttp::doInput(JZHttpServerClient* client, const JZHttpRequest& req,
                                  InputKind kind, const std::string& windowKey)
{
    if (req.method() != JZHttpRequest::POST) {
        replyError(client, req, 405, "只支持 POST");
        return;
    }
    Json::Value body;
    if (!takeBody(client, req, body))
        return;

    if (!windowKey.empty()) {
        // 单窗口那条路要自己说清是鼠标还是键盘: type 显式给最清楚, 没给就按参数猜
        const std::string type = body.get("type", "").asString();
        if (type == "mouse") {
            kind = InputKind::Mouse;
        } else if (type == "key") {
            kind = InputKind::Key;
        } else if (!type.empty()) {
            replyError(client, req, 400, "type 只能是 mouse 或 key, 收到: " + type);
            return;
        } else if (body.isMember("key") || body.isMember("text")) {
            kind = InputKind::Key;
        } else {
            kind = InputKind::Mouse;
        }
    }

    auto box = std::make_shared<Json::Value>(Json::objectValue);
    auto fail = std::make_shared<std::string>();
    auto notFound = std::make_shared<bool>(false);
    const bool ok = onScreen([this, box, fail, notFound, kind, windowKey, body]() {
        JZVirtualWindow* target = nullptr;
        if (!windowKey.empty()) {
            target = findWindow(windowKey);
            if (!target) {
                *fail = "没有这个窗口: " + windowKey;
                *notFound = true;
                return;
            }
        }
        if (kind == InputKind::Mouse)
            doMouseInput(body, target, *box, *fail);
        else
            doKeyInput(body, target, *box, *fail);
    });
    if (!ok) {
        replyError(client, req, 500, "桌面线程没有响应");
        return;
    }
    if (!fail->empty()) {
        replyError(client, req, *notFound ? 404 : 400, *fail);
        return;
    }
    replyJson(client, req, *box);
}

void JZVirtualDeviceHttp::handleMouse(JZHttpServerClient* client, const JZHttpRequest& req)
{
    doInput(client, req, InputKind::Mouse, std::string());
}

void JZVirtualDeviceHttp::handleKey(JZHttpServerClient* client, const JZHttpRequest& req)
{
    doInput(client, req, InputKind::Key, std::string());
}

// 给**这个窗口**直接发鼠标/键盘 (不激活、不过桌面命中): 见头注释"两套输入的区别"
void JZVirtualDeviceHttp::handleWindowInput(JZHttpServerClient* client, const JZHttpRequest& req)
{
    doInput(client, req, InputKind::Mouse, req.param("id"));   // 鼠标/键盘由体里的 type 决定
}


// ==================== 程序 (加载/运行/停止) ====================

void JZVirtualDeviceHttp::handlePrograms(JZHttpServerClient* client, const JZHttpRequest& req)
{
    if (req.method() == JZHttpRequest::GET) {
        if (!m_host.list) {
            replyError(client, req, 501, "宿主没有登记程序管理 (JZVirtualProgramHost)");
            return;
        }
        const std::vector<std::string> names = m_host.list();
        auto box = std::make_shared<Json::Value>(Json::objectValue);
        const bool ok = onScreen([this, box, names]() {
            Json::Value arr(Json::arrayValue);
            for (const std::string& n : names) {
                Json::Value o(Json::objectValue);
                o["name"] = n;
                o["windows"] = windowsOfProgram(n);
                arr.append(o);
            }
            (*box)["ok"] = true;
            (*box)["count"] = (int)names.size();
            (*box)["programs"] = arr;
        });
        if (!ok) {
            replyError(client, req, 500, "桌面线程没有响应");
            return;
        }
        replyJson(client, req, *box);
        return;
    }

    if (req.method() != JZHttpRequest::POST) {
        replyError(client, req, 405, "只支持 GET (列表) 与 POST (加载)");
        return;
    }
    if (!m_host.launch) {
        replyError(client, req, 501, "宿主没有登记程序管理 (JZVirtualProgramHost)");
        return;
    }

    Json::Value body;
    if (!takeBody(client, req, body))
        return;

    const std::string name = body.get("name", "").asString();
    if (name.empty()) {
        replyError(client, req, 400, "要给程序名 name");
        return;
    }
    std::string script, workDir, moduleRel, error;
    if (!resolveProgram(body, script, workDir, moduleRel, error)) {
        replyError(client, req, 400, error);
        return;
    }

    // ⚠ 这一句会起线程、并可能等到它的第一个窗口出现 (见 jzAttachVirtualProgramHost) ——
    //   所以放在**主线程**上直接跑 (onScreen 只保证线程归属, 不额外加锁)。
    if (!m_host.launch(name, script, workDir, moduleRel, error)) {
        replyError(client, req, 400, error.empty() ? "加载程序失败" : error);
        return;
    }

    auto box = std::make_shared<Json::Value>(Json::objectValue);
    const bool ok = onScreen([this, box, name, moduleRel, workDir]() {
        m_screen->refresh();
        (*box)["ok"] = true;
        (*box)["name"] = name;
        (*box)["module"] = moduleRel;                  // 空的 = 跑的是脚本文本
        (*box)["workDir"] = workDir;
        (*box)["windows"] = windowsOfProgram(name);     // 拿到响应就能截图/点击, 不用自己轮询
    });
    if (!ok) {
        replyError(client, req, 500, "桌面线程没有响应");
        return;
    }
    replyJson(client, req, *box);
}

void JZVirtualDeviceHttp::handleProgramEval(JZHttpServerClient* client, const JZHttpRequest& req)
{
    if (req.method() != JZHttpRequest::POST) {
        replyError(client, req, 405, "只支持 POST");
        return;
    }
    if (!m_host.run) {
        replyError(client, req, 501, "宿主没有登记程序管理 (JZVirtualProgramHost)");
        return;
    }
    Json::Value body;
    if (!takeBody(client, req, body))
        return;
    const std::string name = percentDecode(req.param("name"));    // 路径值要自己解码 (见 percentDecode)
    std::string script, workDir, error;
    if (!readScript(body, script, workDir, error)) {
        replyError(client, req, 400, error);
        return;
    }
    if (!m_host.run(name, script, error)) {
        replyError(client, req, 400, error.empty() ? "执行失败" : error);
        return;
    }
    auto box = std::make_shared<Json::Value>(Json::objectValue);
    const bool ok = onScreen([this, box, name]() {
        m_screen->refresh();
        (*box)["ok"] = true;
        (*box)["name"] = name;
        (*box)["windows"] = windowsOfProgram(name);
    });
    if (!ok) {
        replyError(client, req, 500, "桌面线程没有响应");
        return;
    }
    replyJson(client, req, *box);
}

void JZVirtualDeviceHttp::handleProgramStop(JZHttpServerClient* client, const JZHttpRequest& req)
{
    if (req.method() != JZHttpRequest::DELETE) {
        replyError(client, req, 405, "只支持 DELETE");
        return;
    }
    if (!m_host.stop) {
        replyError(client, req, 501, "宿主没有登记程序管理 (JZVirtualProgramHost)");
        return;
    }
    const std::string name = percentDecode(req.param("name"));    // 同上: 路径里的程序名可能带中文
    if (!m_host.stop(name)) {
        replyError(client, req, 404, "没有这个程序: " + name);
        return;
    }
    auto box = std::make_shared<Json::Value>(Json::objectValue);
    const bool ok = onScreen([this, box, name]() {
        m_screen->refresh();
        (*box)["ok"] = true;
        (*box)["name"] = name;
    });
    if (!ok) {
        replyError(client, req, 500, "桌面线程没有响应");
        return;
    }
    replyJson(client, req, *box);
}
