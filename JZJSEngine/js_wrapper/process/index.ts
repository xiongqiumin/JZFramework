/**
 * @jz/process —— Node `process` 模块(本框架的最小实现)。
 *
 * 成员: cwd / execPath / platform / argv + **exit / exitCode**(语义对齐 Node)。
 * 引擎另外还注册了一个**全局 `process`**(见 js_wrapper/types/globals.d.ts), 它和本模块
 * 是**同一套底层**(`__jz.process`): 所以 `process.exit(0)` 不 import 也能用, 行为一致。
 *
 * exit 的参数/退出码规则(逐条与真 Node v26 对照过, 见 build/_codex_bench/api_parity/exit_sem.py):
 *   exit(3) / exit("7") -> 3 / 7          exit() / exit(null) -> exitCode(设过)否则 0
 *   exit(-1) -> 4294967295(uint32 回绕)   exit(1.5)          -> RangeError ERR_OUT_OF_RANGE
 *   exit("x") / exit({}) / exit(true)     -> TypeError ERR_INVALID_ARG_TYPE
 * exitCode 是**可读写属性**: 整数/整数串(读回来是 number); null/undefined 清空(读回 undefined);
 * 非整数数值抛 RangeError、其它类型抛 TypeError。脚本自然结束时它就是进程退出码。
 *
 * ⚠ 已知差异: Node 的 process.exit 会**立即终止**(同一帧后面的语句不再执行), 这里只"请求宿主
 *   退出"(JZJSEngine::requestProcessExit) —— 当前同步帧会跑完, 但之后的定时器/微任务不再执行。
 */
'use strict';

declare const __jz: any;

export function cwd(): string { return __jz.core.cwd(); }
export const execPath: string = __jz.core.execPath();
/** "windows" -> "win32"(与 Node 同形); 是**属性**不是函数 */
export const platform: string = __jz.core.platform();
/** 本框架不解析命令行参数, 恒为空数组(宿主可自行填充) */
export const argv: string[] = [];

/** 见文件头: 参数与退出码规则对齐 Node(校验在引擎侧同一份实现里) */
export function exit(code?: number | string | null): void { __jz.process.exit(code); }

const processObj: any = { cwd, execPath, platform, argv, exit };

// exitCode: Node 里是**可读写属性**(不是函数) ⇒ 用访问器挂上去
Object.defineProperty(processObj, 'exitCode', {
    enumerable: true,
    configurable: true,
    get() { return __jz.process.exitCode(); },
    set(v: number | string | null | undefined) { __jz.process.setExitCode(v); },
});

export default processObj;
