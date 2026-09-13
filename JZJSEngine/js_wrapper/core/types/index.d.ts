/**
 * @jz/core —— 外部定义汇总 (external definitions, public contract).
 *
 * 本包对应 JZJSCoreModule / JZCrypto / JZFetch 等注入的全局模块:
 * fs / path / process / crypto。(fetch / console 为纯全局, 见 js_wrapper/types/。)
 */
export * from './fs';
export * from './path';
export * from './process';
export * from './crypto';
// bytes: 本框架特有的二进制读写工具(非 Node 模块), 见同目录 ./bytes.d.ts
export * from './bytes';
