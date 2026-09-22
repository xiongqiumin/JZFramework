/**
 * @jz/os —— Node `os` 模块。
 *
 * 原子能力来自两处: __jz.os.networkInterfaces() + __jz.core.platform()/tmpdir()
 * (以前这两半分别写在 net 与 core 两个包里, 再由 core 合并 —— 现在合成一个同名包,
 *  不再有跨包合并)。
 * 默认导出 = 模块对象(对齐 Node 的 os 对象), 具名导出 = 各成员。
 */
'use strict';

declare const __jz: any;

export function networkInterfaces() { return __jz.os.networkInterfaces(); }
/** 引擎的 platform():"windows" -> "win32"(与 Node 同形) */
export function platform(): string { return __jz.core.platform(); }
export function tmpdir(): string { return __jz.core.tmpdir(); }
/** "\r\n" on win32, "\n" otherwise */
export const EOL: string = (__jz.core.platform() === 'win32') ? '\r\n' : '\n';

const os = { networkInterfaces, platform, tmpdir, EOL };
export default os;
