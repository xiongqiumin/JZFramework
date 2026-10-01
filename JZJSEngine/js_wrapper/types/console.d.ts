// console - global logger provided by the JZ JS engine at runtime.
// Declared as a global (no module wrapper) so user code can call console.log
// directly without an import. The engine maps these to its own log sink.
// NOTE: the engine only registers `log` / `error` / `warn` / `debugPrint` / `assert`
// (see JZJSEngine::registConsole), so only those are declared here to stay in
// sync with the runtime.
declare const console: {
    /**
     * Node 的 console.log: 多个参数按空格拼接; 顶层字符串不加引号, 容器按 util.inspect 风格
     * 输出 (`{ a: 1 }` / `[ 1, 2 ]` / `Map(1) { 'k' => 1 }` / `[Function: f]` / `[Circular]`),
     * 嵌套深度 2; 首参是字符串时支持 util.format 占位符 `%s %d %i %f %j %o %O %c %%`。
     */
    log(...args: any[]): void;
    /** 同 log (Node 里 log/info 等价; 这里 error/warn 也只是 sink 意图, 实际同一个 sigLog) */
    error(...args: any[]): void;
    /** 同 log */
    warn(...args: any[]): void;
    /**
     * JZ 原来的紧凑格式(**不是** node 的 API): 字符串带引号(`"x"`)、`{a: 1}` 无内边距、
     * 且按 for...in 列出**继承来的可枚举属性** —— 打印 JZ 绑定对象(控件等)时比 log 有信息量。
     * 依赖旧输出 / 需要紧凑单行 / 想看原型上的字段时用它。
     */
    debugPrint(...args: any[]): void;
    /** 对齐 Node 语义: 断言失败只打印, **不抛异常**; 消息拼法也与 Node 一致(前缀 "Assertion failed") */
    assert(condition: any, ...args: any[]): void;
};
