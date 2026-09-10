#include "JZEngineRuntime.h"

#include "JZApplication.h"
#include "JZDir.h"
#include "JZFileInfo.h"
#include "JZJSEngine.h"
#include "JZJSNetModule.h"
#include "JZJSCoreModule.h"
#include "JZPlatform.h"
#include "JZObject.h"
#include "JZString.h"
#include "JZThread.h"
#include "json/json.h"

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

    // 挂载内置模块：net（fetch/http/net/ws...）必须先于 core，core 会复用 net 建好的
    // __jz 命名空间（见 JZJSCoreModule::initEngine），顺序反了会丢失 fetch 等网络全局。
    // 二者共同提供 jz_types 声明的运行期全局（fs/path/process/os/fetch/http/...）。
    m_netModule = std::make_unique<JZJSNetModule>();
    m_netModule->initEngine(engine);
    m_coreModule = std::make_unique<JZJSCoreModule>();
    m_coreModule->initEngine(engine);

    // Node 兼容: 引擎启动即把 net 适配层的 Web 标准全局(fetch/Headers/Response/
    // WebSocket/EventEmitter/URL/URLSearchParams)挂到 globalThis, 用户无需 import 即可
    // 用(等价 Node 启动装载 undici/whatwg)。net 模块按绝对路径加载, 与用户入口
    // `import '../jz_js/net/dist/index.js'` 命中同一模块缓存, 不会重复执行; 找不到
    // shim 时静默跳过(纯算法/无网络工程仍可用)。
    bootstrapWebGlobals(wd);

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

void JZEngineRuntime::bootstrapWebGlobals(const string &wd)
{
    // 候选路径: 优先工程自带的 jz_js(与用户 import 解析到的绝对路径一致, 避免重复执行),
    // 回退到 exe 同目录部署的 jz_engine/jz_js(或旧平铺 jz_js)。路径先规范化(消解 ..),
    // 保证与入口 `import '../jz_js/net/dist/index.js'` 解析到同一文件、命中同一模块缓存。
    vector<string> candidates;
    candidates.push_back(rtAbsPath(wd + "/jz_js/net/dist/index.js"));        // 运行根带 jz_js
    candidates.push_back(rtAbsPath(wd + "/../jz_js/net/dist/index.js"));     // 运行根在 build/ 子目录
    string appDir = JZPlatform::applicationDirPath();
    if (!appDir.empty())
    {
        candidates.push_back(rtAbsPath(appDir + "/jz_engine/jz_js/net/dist/index.js"));  // 新部署
        candidates.push_back(rtAbsPath(appDir + "/jz_js/net/dist/index.js"));           // 旧平铺
    }
    for (const string &cand : candidates)
    {
        if (JZFileInfo(cand).isFile())
        {
            // 副作用执行: 把 Web 标准全局挂上 globalThis。按绝对路径加载, 与用户入口
            // 的 import 命中同一 QuickJS 模块缓存, 不会重复执行。失败不致命(静默)。
            m_engine->runModuleFile(cand);
            break;                                  // 命中即止
        }
    }
}

void JZEngineRuntime::shutdownEngine()
{
    // 先卸载内置模块（释放其持有的引擎引用 / 回调根），再销毁引擎本身
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
    m_root = rtAbsPath(projectDir);
    while (m_root.size() > 2 && str_endsWith(m_root, "/"))
        str_chop(m_root, 1);
    if (m_root.empty())
    {
        m_lastError = "init: 工程根为空";
        return false;
    }

    // 主入口统一为运行根下的 main.js（project.json 只是补充信息，不声明入口）
    std::string main_js = m_root + "/main.js";
    if(!JZFileInfo::exists(main_js))
    {
        m_lastError = "init: 找不到主入口 main.js: " + main_js +
                      "（工程尚未打包/发布？运行根应是 main.js 所在的产物目录）";
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

    // 2) 以 ESM 方式加载并执行入口模块（入口具名导出绑定到全局）
    if (!runModule("main.js"))
        return false;

    return true;
}

bool JZEngineRuntime::hasPendingJob() const
{
    // 没有会话一律视为"没有待执行任务"（init 失败/已关闭时守卫应立即放行退出）
    return m_engine && m_engine->hasPendingJob();
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
