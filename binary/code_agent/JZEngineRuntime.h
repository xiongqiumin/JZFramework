#ifndef JZ_ENGINE_RUNTIME_H_
#define JZ_ENGINE_RUNTIME_H_

#include <functional>
#include <memory>
#include <string>

#include "JZSourceMap.h"

class JZJSEngine;
class JZJSNetModule;
class JZJSCoreModule;
class JZJSqliteModule;
class JZJSModbusModule;
class JZJSWidgetModule;
class JZJSWorkerModule;
class JZJSComputerUseModule;

// ------------------------------------------------------------------
// 运行期层：内嵌 JS 引擎会话的"运行"封装
//
//   只负责运行：init(root[, entry]) 一步到位——把 root 设为运行根（缺省主入口是它下面的
//   main.js，即"产物目录"；TS 工程通常是 tsconfig 的 outDir，如 <工程根>/build），
//   开一个干净的 JZJSEngine 会话、以 ESM 模块方式加载并执行入口（import 依赖
//   自动解析，入口具名导出绑定全局）。给了 entry 就跑那一份 js 当入口（见下面重载）。
//
//   project.json（可选，与 main.js 同目录）不是入口声明，只是补充信息：编译时间、
//   原路径地址（工程根 / 源码目录）等。init 会读它，解析（栈翻译时把源路径还原成
//   "src/x.ts"）会用到里面的 projectRoot；读不到不影响运行。
//
//   出错时对外给的一律是"翻译回 TS 的位置"（原始 JS 栈经 JZSourceMap 翻译，
//   统一从 lastError() 取）。source map 的解析 / 位置反查不在这里，见
//   JZSourceMap——本类只是持有一份翻译器用于把错误栈翻成人能看的位置。
//
//   设计要点：
//     - 不在类外接触 JZJSEngine：init / evalScript 出错时自动把原始 JS 栈
//       翻译回 TS，统一从 lastError() 取（人/模型看到的永远是 TS 位置）；
//     - 每次 init() 都是全新的干净会话（模块缓存/全局互不串），
//       换一个工程根就再 init() 一次；
//     - 不做编译、不认识 TS 语法；不解释栈语义；没有沙箱概念——只认"运行根"
//       （js 与 .map 拷贝到的目录），不关心代码从哪来。
//
//   典型用法：
//         JZEngineRuntime rt;
//         rt.setLogHandler(...);                      // 模块 console 转发（可选）
//         if (rt.init(wsDir))                         // 运行根 + 加载入口 + 自动调用 main()
//              rt.evalScript("...");                  // 复用会话，可用绑定到全局的入口导出
//         else
//              printf("%s", rt.lastError().c_str());  // 已翻译回 TS 源码位置
// ------------------------------------------------------------------
class JZEngineRuntime
{
public:
    // 引擎 console.* 等日志的转发回调
    typedef std::function<void(const std::string &text)> LogHandler;

    // 未处理 JS 异常（引擎 sigError，含异步未捕获拒绝）回调；参数为已翻译回 TS 的错误文本
    typedef std::function<void(const std::string &tsError)> UnhandledHandler;

    // 主入口固定为运行根下的 main.js；运行根下还可以有一份 project.json，
    // 它不是入口声明，只是"补充信息"（打包/发布时生成）：编译时间、原路径地址
    // （工程根 / 源码目录）、产物输出目录等。栈翻译出的源路径是绝对路径，靠里面的
    // projectRoot 才能显示成 "src/x.ts"（见 toDisplayPath）。读不到不影响运行。
    struct ProjectInfo
    {
        std::string name;          // 工程名
        std::string projectRoot;   // 原路径地址：工程根
        std::string srcRoot;       // 原路径地址：源码目录
        std::string outDir;        // 产物输出目录（相对工程根；工程根外为绝对路径）
        std::string buildTime;     // 编译时间（YYYY-MM-DD HH:MM:SS）
        std::string tsc;           // 编译时用的 tsc
        bool valid = false;        // 是否读到了 project.json
    };

    JZEngineRuntime();
    ~JZEngineRuntime();     // 自动 shutdownEngine()

    // ------------------------------------------------------------------
    // 一站式运行入口
    // ------------------------------------------------------------------
    // 相当于：设运行根(projectDir) + 新建干净引擎会话 + 加载并执行主入口模块
    // + 自动调用入口主函数（入口导出的 main，若无 main 则只执行模块顶层）。
    //
    // 哪个模块是"主入口"由工程根下的 project.json 声明（先"打包/发布"才有）：
    //     { "entry": "build/main.js" }   // entry = 产物路径，'/' 分隔，扩展名可省略
    // entry 通常相对 projectDir（产物目录=tsconfig outDir，缺省 build/）；若 outDir
    // 指向工程根之外（如绝对目录），打包(buildProject)会把 entry 写成绝对路径，
    // 运行端两种形态都支持。
    // init() 自己读取该配置；读不到（工程尚未打包/发布）或声明里的产物文件不存在
    // 时直接报错返回 false —— 不猜测、不回退默认入口；运行期 JS 异常的错误仍会
    // 翻译回 TS 位置，统一从 lastError() 取。
    //
    // 换一个工程根就再 init() 一次（每次都是全新会话，模块缓存/全局互不串）。
    bool init(const std::string &work_dir);

    // 指定入口版本：work_dir 仍是运行根（相对路径/读文件以它为准），但主入口换成 entry
    // 指向的那份 js —— 想跑任意一份脚本（而不是产物里的 main.js）时用这个重载。
    // entry 相对 work_dir 给，也可以直接给绝对路径（“可执行的那个文件”由调用方挑好）。
    // 找不到入口文件同样报错返回 false，不回退默认入口。
    bool init(const std::string &work_dir, const std::string &entry);

    // 运行根（上次 init() 设置的目录，即 main.js 所在目录）。栈里的相对模块名按它解析。
    const std::string &root() const;

    // 运行根下 project.json 里的补充信息（见 ProjectInfo）。
    // 没有 project.json 时 valid=false、字段全空。
    const ProjectInfo &projectInfo() const;

    // 把解析出的"源文件绝对路径"转成显示用的相对路径（解析本身只给绝对路径，
    // 相对谁是显示策略，所以放在这里）：
    //   1) 落在运行根下 -> 相对运行根
    //   2) 落在工程根下 -> 相对工程根（"src/x.ts"，project.json 的 projectRoot）
    //   3) 都不在其下   -> 原样绝对路径（不谎报位置）
    std::string toDisplayPath(const std::string &abs) const;

    // 原始 JS 错误栈 -> 翻译后的 TS 栈（位置已按 toDisplayPath 相对化）
    std::string mapErrorStack(const std::string &raw) const;

    // ------------------------------------------------------------------
    // 内嵌 JZJSEngine 低层接口（init() 内部按序调用它们；只做栈翻译 / 复用
    // 会话时可自行组合，见类注释）
    // ------------------------------------------------------------------

    // 启动（或重启）一个干净的引擎会话。workDir 为空时用 root() 作引擎工作目录。
    // 会先关掉已有会话。
    bool initEngine(const std::string &workDir);
    // 关闭当前会话（析构时自动调用）
    void shutdownEngine();
    bool engineRunning() const;

    // 注: Web 标准全局(fetch/Headers/Response/WebSocket/EventEmitter/URL/URLSearchParams)
    // 由 JZJSNetModule 在注册时装载(见 JZJSNetModule::loadWebGlobalModules), 宿主不再参与。
    // 以 ESM 模块方式加载并执行 moduleRel（相对 root()，'/' 分隔，扩展名可省略）。
    // 需要先 initEngine()；入口模块的命名导出会绑定到全局（供后续 evalScript 用）。
    // 成功返回 true；失败返回 false，错误已经过 source map 翻译回 TS，见 lastError()。
    bool runModule(const std::string &moduleRel);

    // 在当前会话里执行一段脚本（可调用 runModule 绑定到全局的入口导出）。
    // 出错返回 false，lastError() 为翻译后的错误文本。
    bool evalScript(const std::string &script);

    // 当前会话是否还有待执行/未决的异步任务（quickjs 微任务队列 / JS 的
    // setTimeout、setInterval 等）。引擎的异步推进依赖事件循环（app.exec /
    // processEvents），可用于"入口同步部分跑完但异步还没结束"时决定是否
    // 留在事件循环里继续泵。没有会话（init 未成功/已关闭）返回 false。
    bool hasPendingJob() const;

    // 最近一次 init / runModule / evalScript 的错误（已翻译回 TS）；成功或尚未运行时为空
    const std::string &lastError() const;

    // 引擎 console.* / 顶层日志转发回调（可选）。未捕获异常的原文不会转发，
    // 它以翻译回 TS 的版本出现在 lastError() 里。
    void setLogHandler(const LogHandler &handler);

    // 设置未处理 JS 异常回调（引擎 sigError 上报时触发，参数为翻译回 TS 的错误文本）。
    // 默认空：不处理。运行时外壳可借此弹窗/退出。
    void setUnhandledExceptionHandler(const UnhandledHandler &handler);

private:
    // 引擎 sigLog 桥（实现见 .cpp）：把 console.* 转给 logHandler，滤掉异常原文
    class LogBridge;
    LogBridge *m_bridge = nullptr;             // 与 m_engine 同生共死（桥先删）
    // 引擎 sigError 桥（实现见 .cpp）：把未处理 JS 异常翻译回 TS 后转给 m_unhandledHandler
    class ErrorBridge;
    ErrorBridge *m_errorBridge = nullptr;      // 与 m_engine 同生共死（桥先删）
    JZJSEngine *m_engine = nullptr;            // 当前会话（initEngine 创建）
    std::unique_ptr<JZJSNetModule> m_netModule;   // 内置网络模块（fetch/http/net/ws...）
    std::unique_ptr<JZJSCoreModule> m_coreModule; // 内置核心模块（fs/path/process/os...）
    std::unique_ptr<JZJSqliteModule> m_sqliteModule; // 内置 SQLite 模块（node:sqlite 风格）
    std::unique_ptr<JZJSModbusModule> m_modbusModule; // 内置 Modbus 模块（modbus-serial 风格）
    std::unique_ptr<JZJSWidgetModule> m_widgetModule; // 内置 widget 模块（窗口/画布 JS 桥）
    std::unique_ptr<JZJSComputerUseModule> m_computerUseModule; // computer use（__jz.computeruse）
    std::unique_ptr<JZJSWorkerModule> m_workerModule; // 内置 worker 模块（__jz.worker + Worker 类）
    std::string m_engineWorkDir;               // 当前会话的工作目录（引擎落点）
    bool m_engineRunning = false;
    std::string m_lastError;                   // 最近一次运行错误（已 mapStack）
    LogHandler m_logHandler;                   // 模块 console 转发
    UnhandledHandler m_unhandledHandler;       // 未处理 JS 异常回调（默认空）

    std::string m_root;
    ProjectInfo m_projectInfo;     // 运行根下 project.json 的补充信息（可缺省）
    // 位置翻译器（JS 产物位置 -> TS 源码位置）：只在出错时把原始栈翻成人能看的
    // 位置，解析 / 反查的实现全在 JZSourceMap 里，不在本类。
    JZSourceMap m_sourceMap;
};

#endif // !JZ_ENGINE_RUNTIME_H_
