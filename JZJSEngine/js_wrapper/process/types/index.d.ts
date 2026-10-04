export function cwd(): string;
export const execPath: string;
export const platform: string;
/** 启动参数 = [宿主可执行文件, 入口模块绝对路径, ...宿主给的参数](语义同 Node) */
export const argv: string[];
/**
 * 退出进程(对齐 Node): 整数/整数串 -> 该退出码; 省略/null -> 用 exitCode(设过)否则 0;
 * 非整数数值抛 RangeError(ERR_OUT_OF_RANGE), 其它类型抛 TypeError(ERR_INVALID_ARG_TYPE)。
 */
export function exit(code?: number | string | null): void;

/** default export = the module object (same usage as Node: import x from 'process') */
declare const _default: {
    cwd: typeof cwd,
    execPath: typeof execPath,
    platform: typeof platform,
    argv: typeof argv,
    exit: typeof exit,
    /** 读写属性(不是函数): 整数/整数串; null/undefined 清空(读回 undefined) */
    exitCode: number | string | null | undefined,
};
export default _default;
