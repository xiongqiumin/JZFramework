#include "JZUiMcpTools.h"
#include "JZMcpCore.h"
#include "JZWindow.h"
#include "JZPushButton.h"
#include "JZSlider.h"
#include "JZSwitch.h"
#include "JZCheckbox.h"
#include "JZRadioButton.h"
#include "JZComboBox.h"
#include "JZListWidget.h"
#include "JZLineEdit.h"
#include "JZTextEdit.h"
#include "JZMouseEvent.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002   // Win8.1+: 含 DWM 合成内容
#endif

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

// 在控件中心合成一次真实鼠标点击 (走 onWindowProcess 完整命中测试链路)
static bool clickWidget(JZWidget* w, std::string* err)
{
    JZWindow* win = w->window();
    if (!win || !win->nativeHandle()) {
        *err = "目标控件不在已创建的窗口内";
        return false;
    }
    JZPoint c = w->mapToGlobal(JZPoint(w->width() / 2, w->height() / 2));
    JZMouseEvent press(JZMouseEvent::Press, JZMouseEvent::LeftButton, c, JZPoint(), 0);
    win->onWindowProcess(&press);
    JZMouseEvent release(JZMouseEvent::Release, JZMouseEvent::LeftButton, c, JZPoint(), 0);
    win->onWindowProcess(&release);
    return true;
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

static int getPngEncoderClsid(CLSID* clsid)
{
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0)
        return -1;
    Gdiplus::ImageCodecInfo* info = (Gdiplus::ImageCodecInfo*)malloc(size);
    Gdiplus::GetImageEncoders(num, size, info);
    int found = -1;
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(info[i].MimeType, L"image/png") == 0) {
            *clsid = info[i].Clsid;
            found = (int)i;
            break;
        }
    }
    free(info);
    return found;
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
    int maxDepth = args.get("maxDepth", -1).asInt();
    Json::Value tree = m_root->dumpSemanticTree(maxDepth);
    sigFinished.emit(id, makeTextResult(tree.toStyledString()));
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
    if (!m_root->nativeHandle()) {
        sigFinished.emit(id, makeTextResult("窗口尚未创建 (未 show)", true));
        return;
    }
    HWND hwnd = (HWND)m_root->nativeHandle();

    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) {
        sigFinished.emit(id, makeTextResult("客户区为空", true));
        return;
    }

    HDC wdc = GetDC(hwnd);
    HDC mem = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);

    // PrintWindow 抓自绘内容 (窗口被遮挡/离屏也正确); 失败退化为 BitBlt 屏幕拷贝
    if (!PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT))
        BitBlt(mem, 0, 0, w, h, wdc, 0, 0, SRCCOPY);

    CLSID clsid;
    if (getPngEncoderClsid(&clsid) < 0) {
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(hwnd, wdc);
        sigFinished.emit(id, makeTextResult("PNG 编码器不可用", true));
        return;
    }

    Gdiplus::Bitmap bitmap(bmp, NULL);   // 需要 GDI+ 已初始化 (渲染后端持有)

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L'\\') + 1);

    static int seq = 0;
    wchar_t name[64];
    swprintf(name, 64, L"ui_shot_%03d.png", ++seq);
    std::wstring full = dir + name;

    bool ok = (bitmap.Save(full.c_str(), &clsid, nullptr) == Gdiplus::Ok);

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd, wdc);

    if (!ok) {
        sigFinished.emit(id, makeTextResult("PNG 保存失败", true));
        return;
    }
    sigFinished.emit(id, makeTextResult("screenshot saved: " + wideToUtf8(full) +
                                        " (" + std::to_string(w) + "x" + std::to_string(h) + " px)"));
}

// ==================== ui_invoke ====================

JZUiInvokeTool::JZUiInvokeTool(JZWindow* root, JZObject* parent)
    : JZMcpTool(parent)
    , m_root(root)
{
    setName("ui_invoke");
    setDescription("驱动控件交互。target: '#viewId' 按对象名, 或 className(role) 如 'button'/'slider'; "
                   "action: click|toggle|setValue|setText|select; value: setValue/setText/select 的参数。"
                   "click 在控件中心合成一次真实鼠标点击 (走完整命中测试)。");
    setInputSchema(makeSchema({{"target", "string"}, {"action", "string"}, {"value", "string"}},
                              {"target", "action"}));
}

void JZUiInvokeTool::run(const Json::Value& id, const Json::Value& args)
{
    std::string target = args.get("target", "").asString();
    std::string action = args.get("action", "").asString();
    const Json::Value& value = args.get("value", Json::Value());

    if (target.empty() || action.empty()) {
        sigFinished.emit(id, makeTextResult("target 与 action 必填", true));
        return;
    }
    JZWidget* w = resolveWidget(m_root, target);
    if (!w) {
        sigFinished.emit(id, makeTextResult("未找到目标: " + target, true));
        return;
    }

    std::string err;
    if (action == "click" || action == "toggle") {
        // click 语义覆盖 button/checkbox/switch/radiobutton/tab (toggle 即点击)
        if (!clickWidget(w, &err)) {
            sigFinished.emit(id, makeTextResult(err, true));
            return;
        }
    } else if (action == "setValue" || action == "setText") {
        if (auto* s = jzobject_cast<JZSlider*>(w)) {
            s->setValue(value.asInt());
            s->sigValueChanged.emit(s->value());
        } else if (auto* le = jzobject_cast<JZLineEdit*>(w)) {
            le->setText(value.asString());
        } else if (auto* te = jzobject_cast<JZTextEdit*>(w)) {
            te->setText(value.asString());
        } else {
            sigFinished.emit(id, makeTextResult(
                "目标 (" + w->aiRole() + ") 不支持 " + action, true));
            return;
        }
    } else if (action == "select") {
        if (auto* cb = jzobject_cast<JZComboBox*>(w)) {
            cb->setCurrentIndex(value.asInt());
            cb->update();
            cb->currentIndexChanged.emit(cb->currentIndex());
            cb->currentTextChanged.emit(cb->currentText());
        } else if (auto* lw = jzobject_cast<JZListWidget*>(w)) {
            lw->setCurrentIndex(value.asInt());
        } else {
            sigFinished.emit(id, makeTextResult(
                "目标 (" + w->aiRole() + ") 不支持 select", true));
            return;
        }
    } else {
        sigFinished.emit(id, makeTextResult("未知动作: " + action, true));
        return;
    }

    sigFinished.emit(id, makeTextResult("invoke ok: " + target + " -> " + action));
}

// ==================== JZUiMcpBridge ====================

JZUiMcpBridge::JZUiMcpBridge(JZWindow* root, JZObject* parent)
    : JZMcpServer(parent)
    , m_root(root)
{
    core()->setServerInfo("JZUi", "0.1.0");
    core()->registerTool(new JZUiGetStateTool(m_root, core()));
    core()->registerTool(new JZUiScreenshotTool(m_root, core()));
    core()->registerTool(new JZUiInvokeTool(m_root, core()));
}
