#include "JZEngineRuntime.h"

#include "JZApplication.h"
#include "JZDir.h"
#include "JZFileInfo.h"
#include "JZJSEngine.h"
#include "JZJSNetModule.h"
#include "JZJSCoreModule.h"
#include "JZJSqliteModule.h"
#include "JZJSModbusModule.h"
#include "JZJSWidgetModule.h"
#include "JZJSComputerUseModule.h"
#include "JZJSCvModule.h"
#include "JZJSOnnxModule.h"
#include "JZJSAudioModule.h"
#ifdef JZ_HAS_EMULATOR
#  include "JZEmulatorViewJs.h"   // 游戏模拟器控件挂到 JS 类层（见该文件头的"依赖方向"说明）
#endif
#include "JZJSWorkerModule.h"
#include "JZPlatform.h"
#include "JZObject.h"
#include "JZString.h"
#include "JZThread.h"
#include "json/json.h"
#include <excpt.h>

using namespace std;

// 引擎 sigLog -> logHandler 的桥。未捕获异常在 onExceptionOccurred 里以原文
// sigLog 上报，与 runtimeError() 完全相同的行滤掉，交给 lastError() 的翻译版。
class JZEngineRuntime::LogBridge : public JZObject
{
public:
    JZEngineRuntime *rt = nullptr;

    void onLog(const string &text)
    {
        if (text.empty() || !rt)
            return;
        if (rt->m_engine)
        {
            string err = rt->m_engine->runtimeError();
            if (!err.empty() && text == err)
                return;
        }
        if (rt->m_logHandler)
            rt->m_logHandler(text);
    }
};

// 引擎 sigError -> 未处理异常回调桥：把原始 JS 错误翻译回 TS 后转给 m_unhandledHandler。
// 翻译本身不在这里做，交给 m_sourceMap（见 JZSourceMap）。
class JZEngineRuntime::ErrorBridge : public JZObject
{
public:
    JZEngineRuntime *rt = nullptr;

    void onError(const string &raw)
    {
        if (rt && rt->m_unhandledHandler && !raw.empty())
        {
            string ts = rt->mapErrorStack(raw);
            rt->m_unhandledHandler(ts);
        }
    }
};

// 让可能挂起的 promise job / console / timer 落定（模块本体是同步求值的）
static void rtPumpEngine()
{
    JZApplication *app = JZApplication::instance();
    for (int i = 0; i < 40; i++)
    {
        if (app)
            app->processEvents();
        JZThread::msleep(5);
    }
}

// ==================== 路径小工具 ====================
// 走框架这套：JZDir 判绝对路径/取绝对路径、JZFileInfo::weaklyCanonicalPath
// 统一分隔符为 '/' 并消除 "." / ".."（不再自己造轮子）。

// 规整成绝对路径：相对量按当前工作目录解析，再消除 "." / ".."。
// 注：JZFileInfo::weaklyCanonicalPath 内部 assert 入参必须是绝对路径，所以先用
// JZDir::absolutePath() 把相对量挂到当前目录上。
static string rtAbsPath(const string &path)
{
    return JZFileInfo::weaklyCanonicalPath(JZDir(path).absolutePath());
}

// 读运行根下 project.json 的补充信息（编译时间 / 原路径地址 ...）。
// 它不是入口声明（主入口固定 main.js），读不到 / 解析失败都返回 valid=false，
// 不影响运行，只是栈翻译时没法把源路径还原成"相对工程根"的形态。
static JZEngineRuntime::ProjectInfo rtReadProjectInfo(const string &root)
{
    JZEngineRuntime::ProjectInfo info;

    JZFileInfo fi(root + "/project.json");
    if (!fi.exists() || !fi.isFile())
        return info;

    JZFile file;
    if (!file.open(root + "/project.json", "rb"))
        return info;
    JZByteArray data = file.readAll();
    string text(data.data(), data.size());
    file.close();

    // 去掉 UTF-8 BOM（Json::Reader 不认）
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
    {
        text.erase(0, 3);
    }

    Json::Value jv;
    Json::Reader reader;
    if (!reader.parse(text, jv) || !jv.isObject())
        return info;

    auto str = [&jv](const char *key) {
        return (jv.isMember(key) && jv[key].isString()) ? jv[key].asString() : string();
    };
    info.name = str("name");
    info.projectRoot = rtAbsPath(str("projectRoot"));
    info.srcRoot = rtAbsPath(str("srcRoot"));
    info.outDir = str("outDir");
    info.buildTime = str("buildTime");
    info.tsc = str("tsc");
    info.valid = true;
    return info;
}

// ==================== JZEngineRuntime ====================
JZEngineRuntime::JZEngineRuntime()
{
}

JZEngineRuntime::~JZEngineRuntime()
{
    shutdownEngine();
}

// ---- 内嵌 JZJSEngine 会话 ----

bool JZEngineRuntime::initEngine(const string &workDir)
{
    shutdownEngine();   // 每次 initEngine 都是全新的干净会话

    string wd = workDir.empty() ? m_root : rtAbsPath(workDir);

    JZJSEngine *engine = new JZJSEngine();
    engine->init();
    engine->setWorkDir(wd);
    engine->start();

    // 供 core 等包以裸模块名(如 'net/dist/index.js')被动 import 兄弟包时, 按运行根与 exe 部署
    // 两处 jz_js 目录定位(与 bootstrapWebGlobals 的 net 候选路径一致)。相对 import 由 QuickJS
    // 归一化为绝对路径, 不经 libraryDirs; 仅裸模块名经此解析。
    {
        std::vector<std::string> libDirs;
        libDirs.push_back(rtAbsPath(wd + "/jz_js"));
        libDirs.push_back(rtAbsPath(wd + "/../jz_js"));
        std::string appDir = JZPlatform::applicationDirPath();
        if (!appDir.empty())
        {
            libDirs.push_back(rtAbsPath(appDir + "/jz_engine/jz_js"));
            libDirs.push_back(rtAbsPath(appDir + "/jz_js"));
        }
        engine->setLibraryDir(libDirs);
    }

    // 挂载内置模块：net（fetch/http/net/ws...）必须先于 core，core 会复用 net 建好的
    // __jz 命名空间（见 JZJSCoreModule::initEngine），顺序反了会丢失 fetch 等网络全局。
    // 二者共同提供 jz_types 声明的运行期全局（fs/path/process/os/fetch/http/...）。
    m_netModule = std::make_unique<JZJSNetModule>();
    m_netModule->initEngine(engine);
    m_coreModule = std::make_unique<JZJSCoreModule>();
    m_coreModule->initEngine(engine);
    // SQLite（node:sqlite 风格）：只注册 __jz.sqlite 原子 API + 原生连接类，
    // JS 适配层(DatabaseSync/StatementSync)由用户入口 import './sqlite/dist/index.js' 被动加载。
    m_sqliteModule = std::make_unique<JZJSqliteModule>();
    m_sqliteModule->initEngine(engine);
    // Modbus（modbus-serial 风格）：注册 __jz.modbus 原子 API + 原生连接类，
    // JS 适配层(ModbusRTU/ModbusTCP)由用户入口 import './modbus/dist/index.js' 被动加载。
    m_modbusModule = std::make_unique<JZJSModbusModule>();
    m_modbusModule->initEngine(engine);

    // widget：把整个 JZWidget 家族(窗口/标签/按钮/布局/画笔...)注册成 JS 全局类, 供 JS 直接
    // 建 GUI; 窗口存在时 runtime 不退出（见 runtime/main.cpp）。类型声明见
    // JZJSEngine/js_wrapper/widget/types/(按 registXxx 一一对应)。
    // 2026-09-13 打开: bteserver 移植(Qt/C++ -> JS)要靠它把 QLabel/QPushButton/... 桥到
    // 框架真控件(见 binary/code_agent/runtime/bteserver/qtcompat.js 的"框架桥")。
    m_widgetModule = std::make_unique<JZJSWidgetModule>();
    m_widgetModule->initEngine(engine);

#ifdef JZ_HAS_EMULATOR
    // 游戏模拟器控件(JZEmulatorView)：它在独立模块里（依赖方向 JZEmulator → JZFramework），
    // 所以"JS 类定义"在框架的 widget 包里、"方法挂载"由这里显式调一次。
    // ⚠ 必须在上面 initEngine 之后 —— 那时全局类才存在，否则 mountTransparentClass 静默跳过。
    jzRegisterEmulatorView(engine);
#endif

    // computer use：__jz.computeruse.*（点击/按键/截图/match + 布局通道 layoutTree/waitStable/launch）。
    // 挂在这里而不是 net/core 里：它要用 JZWidget 的语义树(dumpSemanticTree)与 JZScreenShot，
    // 属于"宿主能力"而不是语言运行时能力。⚠ 必须在 worker 之前装(见下面 net 覆盖 __jz 的说明)。
    m_computerUseModule = std::make_unique<JZJSComputerUseModule>();
    m_computerUseModule->initEngine(engine);

    // OpenCV 图像处理：__jz.cv.*（滤波/颜色转换/形态学/阈值/轮廓/绘制...，见 JZJSCvModule.h）。
    // 和 computeruse 一样是"宿主能力"，挂在同一层；JS 侧适配层在 js_wrapper/cv。
    // ⚠ 没编 OpenCV 的构建（JZ_CU_OPENCV 未定义）也照样装这个模块 —— 它退化成"一律报未编入"，
    //   于是脚本能明确问出 available()=false，而不是 import 直接失败（形状不一致更难查）。
    m_cvModule = std::make_unique<JZJSCvModule>();
    m_cvModule->initEngine(engine);

    // ONNX Runtime 推理：__jz.onnx.*（加载任意 .onnx、看输入输出、喂张量拿张量，见 JZJSOnnxModule.h）。
    // 与 cv 一样"没编也照样装" —— 退化成一律报未编入, 于是脚本能明确问出 available()=false,
    // 而不是 import 就失败(形状不一致更难查)。
    m_onnxModule = std::make_unique<JZJSOnnxModule>();
    m_onnxModule->initEngine(engine);

    // 音频：__jz.audio.*（录/放/设备/WAV/重采样，见 JZJSAudioModule.h）。
    // 同样"没编也照样装"：HAS_JZAudio 未定义时退化成 available()=false，脚本能明确问出来。
    // JS 侧适配层在 js_wrapper/audio（拉模式：C++ 侧排队，JS 用 readInput 取，事件式由包装层轮询提供）。
    m_audioModule = std::make_unique<JZJSAudioModule>();
    m_audioModule->initEngine(engine);

    // worker：__jz.worker.* + 全局 Worker 类（每个 worker 一个独立引擎 + 消息通信）。
    // 【必须最后装】JZJSNetModule::initEngine 会无条件新建并覆盖全局 __jz，而本模块
    // 往 __jz 里塞 worker 子命名空间 —— 排在 net 之前会被覆盖掉。
    // 主线程侧 id 固定 0；parentId = -1 表示没有父 worker。
    m_workerModule = std::make_unique<JZJSWorkerModule>();
    m_workerModule->initEngine(engine, 0, -1, JZByteArray());

    // Node 兼容的 Web 标准全局(fetch/Headers/Response/WebSocket/EventEmitter/URL/URLSearchParams)
    // 不在这里装了 —— 由 JZJSNetModule 在注册时自己装载(见 loadWebGlobalModules),
    // 宿主不必再关心包部署在哪, 也不会重复求值。
    LogBridge *bridge = new LogBridge();
    bridge->rt = this;
    connect(&engine->sigLog, bridge, &LogBridge::onLog);

    m_errorBridge = new ErrorBridge();
    m_errorBridge->rt = this;
    connect(&engine->sigError, m_errorBridge, &ErrorBridge::onError);

    m_engine = engine;
    m_bridge = bridge;
    m_engineWorkDir = wd;
    m_engineRunning = true;

    m_lastError.clear();
    return true;
}

// 注(2026-09-23): 原来这里有 bootstrapWebGlobals() —— 宿主按绝对路径探测 jz_js/{events,url,ws,http}
// 再 runModuleFile。它有三个问题: ① 只在 code_runtime 侧跑, 别的宿主/worker 引擎没有;
// ② 依赖磁盘布局(产物没重编或 jz_js 没部署 → 漏装 fetch, 只报 "fetch is not defined");
// ③ 按**绝对路径**加载 ⇒ 模块缓存键与 import 链路('events/dist/index.js')不同, 每个包被求值两份
//    (globalThis.Headers/WebSocket 被后加载的那份覆盖, 跨份 instanceof 失败; 已实测复现)。
// 现在改由 JZJSNetModule::loadWebGlobalModules() 在**注册时**按契约相对名装载(与 Node 启动装
// undici 同义), 宿主换谁跑都一样。这里不再需要任何 host 端动作。

void JZEngineRuntime::shutdownEngine()
{
    // 先卸载内置模块（释放其持有的引擎引用 / 回调根），再销毁引擎本身。
    // worker 模块最先卸载：它会停掉并回收所有在跑的 worker 线程。
    if (m_workerModule)
    {
        m_workerModule->deinitEngine();
        m_workerModule.reset();
    }
    if (m_netModule)
    {
        m_netModule->deinitEngine();
        m_netModule.reset();
    }
    if (m_coreModule)
    {
        m_coreModule->deinitEngine();
        m_coreModule.reset();
    }
    if (m_sqliteModule)
    {
        m_sqliteModule->deinitEngine();
        m_sqliteModule.reset();
    }
    if (m_modbusModule)
    {
        m_modbusModule->deinitEngine();
        m_modbusModule.reset();
    }
    if (m_computerUseModule)
    {
        m_computerUseModule->deinitEngine();
        m_computerUseModule.reset();
    }
    if (m_cvModule)
    {
        m_cvModule->deinitEngine();     // 会清掉句柄表里挂着的所有 cv::Mat
        m_cvModule.reset();
    }
    if (m_onnxModule)
    {
        m_onnxModule->deinitEngine();   // 会 Release 掉所有已加载的 OrtSession
        m_onnxModule.reset();
    }
    if (m_audioModule)
    {
        m_audioModule->deinitEngine();  // 会关掉所有打开的输入/输出设备
        m_audioModule.reset();
    }
if (m_widgetModule)
{
    m_widgetModule->deinitEngine();
    m_widgetModule.reset();
}
    if (m_bridge)
    {
        delete m_bridge;
        m_bridge = nullptr;
    }
    if (m_errorBridge)
    {
        delete m_errorBridge;
        m_errorBridge = nullptr;
    }
    if (m_engine)
    {
        m_engine->deinit();
        delete m_engine;
        m_engine = nullptr;
    }
    m_engineWorkDir.clear();
    m_engineRunning = false;
    m_lastError.clear();
}

bool JZEngineRuntime::engineRunning() const
{
    return m_engineRunning && m_engine != nullptr;
}

bool JZEngineRuntime::runModule(const string& moduleRel)
{
    if (!m_engine)
    {
        m_lastError = "runModule: 引擎未启动，请先 initEngine()";
        return false;
    }

    m_lastError.clear();
    string errBefore = m_engine->runtimeError();
    // 这才是**用户入口**(引擎启动时预加载的 net/dist/index.js 不算): appDir/appFile 以它为准,
    // 供移植代码用 QApplication.applicationDirPath() 定位随脚本走的资源。
    m_engine->setAppFile(rtAbsPath(moduleRel));
    bool loaded = m_engine->runModuleFile(moduleRel);
    rtPumpEngine();     // 让可能挂起的 job / console 输出落定

    string raw = m_engine->runtimeError();
    if (loaded && raw == errBefore)
        return true;

    // 模块顶层抛错会转为 promise 拒绝：runModuleFile 可能仍返回 true，
    // 以 runtimeError 是否变化为准，并整体翻译回 TS（JZSourceMap 解析 + 本类相对化）
    m_lastError = mapErrorStack(raw.empty() ? errBefore : raw);
    return false;
}

bool JZEngineRuntime::evalScript(const string& script)
{
    if (!m_engine)
    {
        m_lastError = "evalScript: 引擎未启动，请先 initEngine()";
        return false;
    }

    m_lastError.clear();
    string errBefore = m_engine->runtimeError();
    m_engine->eval(script);

    string raw = m_engine->runtimeError();
    if (raw == errBefore)
        return true;

    m_lastError = mapErrorStack(raw);
    return false;
}

const string& JZEngineRuntime::lastError() const
{
    return m_lastError;
}

void JZEngineRuntime::setLogHandler(const LogHandler& handler)
{
    m_logHandler = handler;
}

void JZEngineRuntime::setUnhandledExceptionHandler(const UnhandledHandler& handler)
{
    m_unhandledHandler = handler;
}

bool JZEngineRuntime::init(const string& projectDir)
{
    return init(projectDir, "main.js");
}

bool JZEngineRuntime::init(const string& projectDir, const string& entry)
{
    m_root = rtAbsPath(projectDir);
    while (m_root.size() > 2 && str_endsWith(m_root, "/"))
        str_chop(m_root, 1);
    if (m_root.empty())
    {
        m_lastError = "init: 工程根为空";
        return false;
    }
    if (entry.empty())
    {
        m_lastError = "init: 入口为空";
        return false;
    }

    // 主入口：缺省是运行根下的 main.js（project.json 只是补充信息，不声明入口）；
    // 显式给了 entry 就跑那一份（相对运行根，也可以是绝对路径——调用方挑好的文件）。
    std::string abs_entry = JZFileInfo::exists(entry) ? rtAbsPath(entry) : rtAbsPath(m_root + "/" + entry);
    if(!JZFileInfo::exists(abs_entry))
    {
        m_lastError = "init: 找不到主入口: " + abs_entry +
                      (entry == "main.js"
                           ? "（工程尚未打包/发布？运行根应是 main.js 所在的产物目录）"
                           : "（--entry 给的这份 js 不存在）");
        return false;
    }

    // 运行根换了：翻译器按新根解析栈里的相对模块名，旧的 map 缓存一并作废。
    // project.json 的"原路径地址"只用于显示（toDisplayPath），不参与解析。
    m_projectInfo = rtReadProjectInfo(m_root);
    m_sourceMap.setRoot(m_root);
    m_sourceMap.clearCache();

    // 1) 新建干净会话（会自动先关掉旧的）
    if (!initEngine(m_root))
    {
        if (m_lastError.empty())
            m_lastError = "init: 引擎启动失败";
        return false;
    }

    // 2) 以 ESM 方式加载并执行入口模块（入口具名导出绑定到全局）。
    //    传绝对路径：入口里 './xxx.js' 这种相对 import 会以它为基准归一化，
    //    给相对名则会按运行根归一化（两种都对，前者与磁盘上的真实位置一致）。
    if (!runModule(abs_entry))
        return false;

    return true;
}

bool JZEngineRuntime::hasPendingJob() const
{
    // 没有会话一律视为"没有待执行任务"（init 失败/已关闭时守卫应立即放行退出）
    return m_engine && m_engine->hasPendingJob();
}

// process.exitCode：脚本里设过、且没有走 process.exit(n) 时，宿主在自然结束时把它当进程退出码。
// （process.exit(n) 那条第路线是引擎直接 jzApp->exit(n)，app.exec() 的返回值就是 n。）
int JZEngineRuntime::processExitCode() const
{
    return m_engine ? m_engine->processExitCode() : 0;
}

bool JZEngineRuntime::processExitCodeSet() const
{
    return m_engine && m_engine->processExitCodeSet();
}

const string &JZEngineRuntime::root() const
{
    return m_root;
}

const JZEngineRuntime::ProjectInfo &JZEngineRuntime::projectInfo() const
{
    return m_projectInfo;
}

// 显示策略：解析给的是规范绝对路径，这里按"运行根 -> 工程根 -> 原样"压成相对路径
string JZEngineRuntime::toDisplayPath(const string &abs) const
{
    const string *bases[2] = { &m_root, &m_projectInfo.projectRoot };
    for (int i = 0; i < 2; i++)
    {
        const string &base = *bases[i];
        if (base.empty())
            continue;
        if (abs.size() > base.size() && abs.compare(0, base.size(), base) == 0 &&
            abs[base.size()] == '/')
        {
            return abs.substr(base.size() + 1);
        }
    }
    // 都不落在其下（源码在工程根之外，或没有 project.json）：原样输出，不谎报位置
    return abs;
}

string JZEngineRuntime::mapErrorStack(const string &raw) const
{
    return m_sourceMap.mapStack(raw, [this](const string &abs) {
        return toDisplayPath(abs);
    });
}
