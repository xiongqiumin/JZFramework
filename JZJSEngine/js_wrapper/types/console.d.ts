// console - global logger provided by the JZ JS engine at runtime.
// Declared as a global (no module wrapper) so user code can call console.log
// directly without an import. The engine maps these to its own log sink.
// NOTE: the engine only registers `log` / `error` / `warn` (see JZJSEngine::registConsole),
// so only those three are declared here to stay in sync with the runtime.
declare const console: {
    log(...args: any[]): void;
    error(...args: any[]): void;
    warn(...args: any[]): void;
};
