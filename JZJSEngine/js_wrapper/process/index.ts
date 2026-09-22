/**
 * @jz/process —— Node `process` 模块(本框架的最小实现)。
 *
 * ⚠ 与 Node 的差异(实测口径): 只有 cwd / execPath / platform / argv 四个成员,
 *   **没有 exit / env / argv 之外的进程能力**(退出请让脚本自然结束或由宿主停止)。
 *   另外引擎还注册了一个**遗留全局 process**(只有 cwd, 见 JZJSEngine::registConsole),
 *   不要依赖它 —— 明确依赖请 import 本模块。
 */
'use strict';

declare const __jz: any;

export function cwd(): string { return __jz.core.cwd(); }
export const execPath: string = __jz.core.execPath();
/** "windows" -> "win32"(与 Node 同形); 是**属性**不是函数 */
export const platform: string = __jz.core.platform();
/** 本框架不解析命令行参数, 恒为空数组(宿主可自行填充) */
export const argv: string[] = [];

const process = { cwd, execPath, platform, argv };
export default process;
