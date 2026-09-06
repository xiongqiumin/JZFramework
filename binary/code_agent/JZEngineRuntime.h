#ifndef JZ_ENGINE_RUNTIME_H_
#define JZ_ENGINE_RUNTIME_H_

#include <functional>
#include <map>
#include <string>
#include <vector>

class JZJSEngine;

// ------------------------------------------------------------------
// 运行期层：内嵌 JS 引擎 + "JS 运行栈 -> TS 源码栈" 的位置翻译
//
//   一件事的两个面：
//   1) 运行：init(projectDir) 一步到位——把 projectDir 设为运行根、开一个干净
//      的 JZJSEngine 会话、按工程配置声明的入口加载主模块（import 依赖自动
//      解析，入口具名导出绑定全局），最后自动调用入口主函数 main()。
//      之后仍可 evalScript() 复用同一会话。
//   2) 翻译：引擎(quickjs)报错给的是运行目录里 xx.js 的行列，而人与模型关心
//      的是 xx.ts 的行列。中间只隔一份 tsc 生成的 source map
//      （xx.js.map，tsconfig 需开 "sourceMap": true）。本类按 source map v3
//      规范做"生成位置 -> 原始位置"反查。
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


    // 翻译后的源码位置
    struct Location
    {
        std::string file;   // 源文件（能表示成运行根的相对路径就给相对路径，否则给原样路径）
        int line = 0;       // 1-based
        int column = 0;     // 1-based
        std::string name;   // map 里带的原始标识符（可能为空）
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
    bool init(const std::string &projectDir);

    // 运行根（上次 init() 设置的工程根）。栈里的相对路径按它解析；
    // 源路径尽量显示成它的相对路径。为空时 js 路径按原样解析。
    const std::string &root() const;

    // 映射成功时是否附带原始 js 位置
    //   false(默认): "src/sort.ts:12:7"
    //   true       : "src/sort.ts:12:7 (js: build/sort.js:8:5)"
    // 映射失败时无论该开关如何，都保持原文不动。
    void setKeepOriginal(bool keep);
    bool keepOriginal() const;

    // 清空已加载的 map 缓存（产物重新编译后调用，否则会读到旧映射）
    void clearCache();

    // 整段文本：把其中所有 "<xxx.js>:行:列" 的帧逐个尝试翻译；
    // 找不到 map 或该位置无映射的帧保持原样，不影响其它帧。
    std::string mapStack(const std::string &text) const;

    // 单帧翻译：jsFile 为 js 产物路径（绝对，或相对运行根）；
    // line / column 与 V8 / QuickJS 栈一致，均为 1-based。
    bool mapLocation(const std::string &jsFile, int line, int column, Location &out) const;

    // ------------------------------------------------------------------
    // 内嵌 JZJSEngine 低层接口（init() 内部按序调用它们；只做栈翻译 / 复用
    // 会话时可自行组合，见类注释）
    // ------------------------------------------------------------------

    // 启动（或重启）一个干净的引擎会话。workDir 为空时用 root() 作引擎工作目录。
    // 会先关掉已有会话。
    bool initEngine(const std::string &workDir = std::string());
    // 关闭当前会话（析构时自动调用）
    void shutdownEngine();
    bool engineRunning() const;

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

protected:
    // source map 中一个"段"（同一生成行内按生成列递增）
    struct Segment
    {
        int genColumn = 0;      // 生成文件列（0-based）
        int srcIndex = 0;       // sources 下标
        int srcLine = 0;        // 源文件行（0-based）
        int srcColumn = 0;      // 源文件列（0-based）
        int nameIndex = -1;     // names 下标，-1 表示无
    };

    struct SourceMap
    {
        bool valid = false;     // 解析成功（false 也会被缓存，避免反复读盘）
        std::string mapDir;     // map 所在目录，用于解析 sources / sourceRoot
        std::string sourceRoot;
        std::vector<std::string> sources;
        std::vector<std::string> names;
        std::vector<std::vector<Segment> > lines;   // 下标 = 生成文件行号(0-based)
    };

    // 取 js 产物对应的 map（带缓存）；没有可用 map 返回 nullptr
    const SourceMap *findMap(const std::string &jsFile) const;

    static bool parseMapFile(const std::string &mapPath, SourceMap &out);
    static bool parseMappings(const std::string &mappings, SourceMap &out);
    static bool decodeVLQ(const std::string &s, size_t &pos, int &value);

    std::string resolveSource(const SourceMap &map, int srcIndex) const;
    std::string relativeToRoot(const std::string &path) const;

private:
    // 引擎 sigLog 桥（实现见 .cpp）：把 console.* 转给 logHandler，滤掉异常原文
    class LogBridge;
    LogBridge *m_bridge = nullptr;             // 与 m_engine 同生共死（桥先删）
    JZJSEngine *m_engine = nullptr;            // 当前会话（initEngine 创建）
    std::string m_engineWorkDir;               // 当前会话的工作目录（引擎落点）
    bool m_engineRunning = false;
    std::string m_lastError;                   // 最近一次运行错误（已 mapStack）
    LogHandler m_logHandler;                   // 模块 console 转发

    std::string m_root;
    bool m_keepOriginal = false;
    mutable std::map<std::string, SourceMap> m_maps;   // 规范化后的 js 路径 -> map
};

#endif // !JZ_ENGINE_RUNTIME_H_
