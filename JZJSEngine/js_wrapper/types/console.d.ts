// console - global logger provided by the JZ JS engine at runtime.
// Declared as a global (no module wrapper) so user code can call console.log
// directly without an import. The engine maps these to its own log sink.
// NOTE: the engine only registers `log` / `error` / `warn` / `assert`
// (see JZJSEngine::registConsole), so only those are declared here to stay in
// sync with the runtime.
declare const console: {
    log(...args: any[]): void;
    error(...args: any[]): void;
    warn(...args: any[]): void;
    /** 对齐 Node 语义: 断言失败只打印, **不抛异常** */
    assert(condition: any, ...args: any[]): void;
};
