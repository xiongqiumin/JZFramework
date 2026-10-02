#ifndef JZ_VIRTUAL_DEVICE_HTTP_H_
#define JZ_VIRTUAL_DEVICE_HTTP_H_
#include "JZObject.h"
#include "json/json.h"      // 请求/响应都是 JSON (与框架其它 HTTP/MCP 面同一套: jsoncpp)
#include <functional>
#include <string>
#include <vector>

class JZVirtualDevice;
class JZVirtualWindow;
class JZHttpServer;
class JZHttpServerClient;
class JZHttpRequest;

// ==================== 虚拟桌面的 HTTP 接口 (把这块桌面当"容器"用) ====================
// 一句话: 一台**能被程序驱动的小电脑** —— 加载程序 / 截图 / 收鼠标键盘, 全走 HTTP + JSON;
// 不需要宿主界面, 也不需要真桌面 (`virtual_device.exe --headless --http-port N`)。
//
// 为什么要有它 (与 test_virtual_device / JZComputerUse 的分工):
//   · 桌面本身已经是"虚拟显示器 + 窗口管理器 + 窗口宿主" (见 JZVirtualDevice): 合成/命中/
//     注入/取帧都是现成的; 本类只做**协议**那一层 —— HTTP 路由 + JSON 编解码 + 线程归属;
//   · 于是"被驱动的程序"与"驱动它的脚本"不必同进程、不必同语言 —— 这正是 computer-use
//     那条链最需要的一步: 被测程序在容器里跑, 观察与操作从外面来。
//
// 五条纪律 (都是踩过的坑, 改之前先读):
//   1. **坐标恒为桌面逻辑像素** (与 JZVirtualDevice 同一口径): `scale` 只决定截图有多少像素,
//      不进坐标 ⇒ "点 (100,80)" 在任何 DPI 下都是同一件事, 脚本可复现;
//   2. **截图能给窗口客户区** (`?window=`): 客户区坐标系才与控件几何/命中一致 (标题栏/边框
//      属于窗口管理器, 不是被测程序的东西) —— 与 computer_use 的截图口径一致;
//   3. **不提供"控件树"接口**: 语义树就是视觉评测的答案本身 (`get_ui_state`) —— 给了就等于
//      开卷。容器只给**像素 + 窗口几何** (标题/外框/客户区), 与 `ui_windows` 同口径;
//   4. **桌面操作一律落在主线程**: HTTP 回调本来就在 server 所在线程 (本对象在哪建就在哪
//      响应) 上跑, 这里再兜一层同步投递 —— 于是"注入 -> 合成 -> 截图"是一条确定的链,
//      不会拍到半帧;
//   5. **加载程序由宿主登记** (`JZVirtualProgramHost`): 本层不认识 JS 引擎/进程库 (方向是
//      jz_virtual_process -> jz_virtual_device, 只能单向), 与 addStartMenuItem 同一套哲学
//      —— 容器提供通路, 能力由宿主给。没登记时程序接口一律 501, 绝不假装能跑程序。
//
// 接口一览 (响应体一律 JSON: `{"ok":true,...}` / `{"ok":false,"error":"..."}`; 仅截图是二进制):
//   GET    /api/health                            容器状态 (桌面尺寸/缩放/窗口数/程序数)
//   GET    /api/windows[?process=&title=&visible=&at=x,y]
//                                                 窗口表 (z 序): 标题 + 外框/客户区几何 + 归属程序;
//                                                 可按程序/标题过滤, at=x,y 等价 WindowFromPoint
//   GET    /api/windows/<w>                       查单个窗口 (w = 下标 | #下标 | 0x句柄 | 标题片段)
//   POST   /api/windows         {w,h,title,x?,y?} 新建一个空窗口 (没有 JS 也能用容器)
//   POST   /api/windows/<w>/state  {action,x,y,w,h,title}
//                                                 SetWindowPos 那一组: 激活/最小化/最大化/还原/关闭
//                                                 + 移动 (x,y=外框坐标) / 改客户区尺寸 (w,h) / 改标题
//   POST   /api/windows/<w>/input  {type,action,x,y,...}
//                                                 给**这个窗口**直接发鼠标/键盘 (不激活、不过桌面命中),
//                                                 与 /api/input/* 同一套参数, 见下面 coords 说明
//   DELETE /api/windows/<w>                       关掉它 (== /state {"action":"close"})
//   GET    /api/screen[?window=<w>][&format=png|json]  截图: 默认整屏 PNG; window 给**客户区**
//   POST   /api/desktop         {scale}           改渲染缩放 (只影响截图分辨率, 不进坐标)
//   POST   /api/input/mouse     {action,x,y,...}  鼠标: move/down/up/click/double/drag/wheel
//   POST   /api/input/key       {key|text,...}    键盘: 单键/组合 ("Ctrl+C") 或整段文本
//   POST   /api/programs        {name,file|script,workDir?}  加载程序 (一个线程 + 一份 JS 运行时):
//                                                 file   = 一个 **JS 程序** (按 ESM 模块加载:
//                                                          import 可用, 运行根 = 它所在目录,
//                                                          workDir 缺省也取它所在目录)
//                                                 script = 一段脚本 (直接跑, 无模块语义)
//                                                 ⚠ 程序**起来就崩** (未处理异常/初始化失败)
//                                                    回 400 + 错误文本 —— 不是 ok + 空窗口列表
//   GET    /api/programs                          程序表 (名字 + 它建的窗口)
//   DELETE /api/programs/<name>                   停掉程序 (连带收走它的窗口)
//
// ⚠ **程序的未处理异常 = 那个程序退出** (与真系统同语义): 它从 /api/programs 消失、窗口一起
//   收走, 容器不退、别的程序不受影响 (见 JZVirtualProcess.h 的"未处理异常 = 进程退出")。
//
// 两套输入的区别 (与真系统同一套对照):
//   · 走桌面 (/api/input/*): 坐标 = **桌面**逻辑坐标 —— 命中测试 -> 激活那个窗口 ->
//     按命中区域分发 (标题栏=拖动/边框=缩放/按钮=窗管/客户区=投给控件)。等价"人拿着鼠标点"。
//   · 走窗口 (/api/windows/<w>/input): 坐标默认 = **客户区** (与 PostMessage 的 lParam 同口径,
//     想用桌面坐标写 "coords":"screen"); **不激活窗口**, 直接投给它的控件树。等价 SendMessage。
//     用途: 操作后台窗口、多窗口并行摆弄、脚本里已经算好客户区坐标的情况。
//
// 窗口标识 <w> 有四种写法, 从稳到稳: `0x...` 句柄 (窗口生命期唯一, 与 HWND 同角色) >
// 标题片段 (按标题找最上层那个) > `#下标` / 下标 (z 序位置, 会随建窗/关窗变) ——
// 写成"先枚举再点下标"的脚本要留意下标会变, 长期引用请用句柄。
// ⚠ URL 里 `#` 是片段分隔符 (客户端会把它之后的部分丢掉), 要写 `%23` 或干脆只写数字。
//
// 线程: 本对象**在哪个线程建就在哪个线程响应** (JZHttpServer 的会话会被移到它所在线程)。
// 宿主程序建在主线程 ⇒ 回调也主线程 ⇒ 与桌面同线程, 一切直来直去 (投递只是兜底)。

// 宿主侧的"程序管理" (见上面纪律第 5 条): 三个回调都可以不给 —— 没给的那个接口回 501。
// `jz_virtual_process` 已提供现成实现: `jzAttachVirtualProgramHost(desk, api)` (一行接上)。
struct JZVirtualProgramHost
{
    // 加载程序: 起一个"程序" (name 已存在时应返回 false + error)
    //   moduleRel 非空 = 以 **ESM 模块**加载 (相对 workDir): 真程序走这条 (import 可用,
    //   jzApp.applicationDirPath() = 程序所在目录); 否则 script 当一段 JS 跑。
    std::function<bool(const std::string& name, const std::string& script,
                       const std::string& workDir, const std::string& moduleRel,
                       std::string& error)> launch;
    // 程序名列表 (GET /api/programs)
    std::function<std::vector<std::string>()> list;
    // 停掉一个程序 (连带它建的窗口)
    std::function<bool(const std::string& name)> stop;
};

class JZVirtualDeviceHttp : public JZObject
{
public:
    explicit JZVirtualDeviceHttp(JZVirtualDevice* screen, JZObject* parent = nullptr);
    ~JZVirtualDeviceHttp() override;

    // 起监听。⚠ 默认只绑 127.0.0.1: 这是"本机的自动化接口", 不该挂到网络上。
    bool listen(int port, const std::string& ip = "127.0.0.1");
    void close();
    bool isListening() const;
    int port() const { return m_port; }

    void setProgramHost(const JZVirtualProgramHost& host);
    const JZVirtualProgramHost& programHost() const { return m_host; }

private:
    using Handler = std::function<void(JZHttpServerClient*, const JZHttpRequest&)>;

    void addRoute(const std::string& path, Handler fn);

    // ---- 各端点 ----
    void handleHealth(JZHttpServerClient* client, const JZHttpRequest& req);
    void handleWindows(JZHttpServerClient* client, const JZHttpRequest& req);   // GET 列表 / POST 新建
    void handleWindow(JZHttpServerClient* client, const JZHttpRequest& req);    // GET 单个 / DELETE 关掉
    void handleWindowActivate(JZHttpServerClient* client, const JZHttpRequest& req);
    void handleWindowState(JZHttpServerClient* client, const JZHttpRequest& req);
    void handleScreen(JZHttpServerClient* client, const JZHttpRequest& req);
    void handleDesktop(JZHttpServerClient* client, const JZHttpRequest& req);
    // 鼠标/键盘: 一套解析两处用 —— target 为空 = 走桌面 (命中+激活), 非空 = 直接给那个窗口。
    // 见头注释"两套输入的区别"; windowKey 是路由参数里的窗口标识 (在 onScreen 里才解析)。
    enum class InputKind { Mouse, Key };
    void handleMouse(JZHttpServerClient* client, const JZHttpRequest& req);      // /api/input/mouse
    void handleKey(JZHttpServerClient* client, const JZHttpRequest& req);        // /api/input/key
    void handleWindowInput(JZHttpServerClient* client, const JZHttpRequest& req); // /api/windows/<w>/input
    void doInput(JZHttpServerClient* client, const JZHttpRequest& req, InputKind kind,
                 const std::string& windowKey);
    // 解析 + 投递 (只在主线程里跑, target 为空 = 走桌面): 结果写进 out, 失败写 error
    void doMouseInput(const Json::Value& body, JZVirtualWindow* target,
                      Json::Value& out, std::string& error);
    void doKeyInput(const Json::Value& body, JZVirtualWindow* target,
                    Json::Value& out, std::string& error);
    void handlePrograms(JZHttpServerClient* client, const JZHttpRequest& req);  // GET 列表 / POST 加载
    void handleProgramStop(JZHttpServerClient* client, const JZHttpRequest& req);

    // ---- 内部小工具 ----
    void replyJson(JZHttpServerClient* client, const JZHttpRequest& req,
                   const Json::Value& value, int code = 200);
    void replyError(JZHttpServerClient* client, const JZHttpRequest& req,
                    int code, const std::string& message);
    // 在主线程上跑一段桌面操作; 返回 false = 桌面线程没响应 (超时)
    // ⚠ fn 捕获的东西必须活得比"可能超时的等待"更久 (纪律见 JZVirtualThread.h) ——
    //   所以往 shared_ptr 里写结果, 别捕调用方的栈变量。
    bool onScreen(const std::function<void()>& fn);
    // 窗口标识 -> 窗口: "0x句柄" / "#下标" / "下标" / "标题片段" (找不到 = nullptr), 见头注释
    JZVirtualWindow* findWindow(const std::string& key) const;
    Json::Value windowJson(JZVirtualWindow* w, int index) const;
    // 某个程序建的窗口 (先按对象名找到它的进程, 再问桌面 windowsOfProcess; 见 JZVirtualProcess)
    Json::Value windowsOfProgram(const std::string& name) const;
    // "加载什么" 的两种写法 (见 /api/programs 的说明):
    //   file   -> moduleRel = 文件名 (相对 workDir) + workDir 缺省 = 文件所在目录 (按模块加载)
    //   script -> script = 那段代码 (直接跑)
    bool resolveProgram(const Json::Value& body, std::string& script, std::string& workDir,
                        std::string& moduleRel, std::string& error) const;

    JZVirtualDevice* m_screen = nullptr;
    JZHttpServer* m_http = nullptr;      // 监听 + 路由 (handler 挂它名下, 随本对象析构)
    int m_port = 0;
    JZVirtualProgramHost m_host;         // 宿主登记的程序管理 (见头注释第 5 条)
};

#endif // JZ_VIRTUAL_DEVICE_HTTP_H_
