/**
 * @jz/core —— 外部定义汇总 (external definitions, public contract).
 *
 * 本包对应 JZJSCoreModule / JZCrypto / JZFetch 等注入的全局模块:
 * fs / path / process / crypto。(fetch / console 为纯全局, 见根 types/。)
 */
export * from './fs';
export * from './path';
export * from './process';
export * from './crypto';
