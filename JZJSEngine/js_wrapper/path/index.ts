/**
 * @jz/path —— Node `path` 模块(纯字符串处理, 不碰文件系统)。
 *
 * 走 __jz.core 的 fileName/dirName/extName/isAbsolute 原子能力(所以路径分隔符语义与引擎一致)。
 * 默认导出 = 模块对象(对齐 Node 的 path 对象), 具名导出 = 各函数。
 */
'use strict';

declare const __jz: any;

// 本框架所有路径在 C++ 侧都归一化成 '/' (JZFileInfo/JZDir/JZPlatformWin32 都 str_replace("\\","/")),
// 所以 path.sep 也固定为 '/', 与 join/normalize/resolve 的输出一致 —— 这点与 Node 在 Windows 上
// 返回 '\\' 不同, 但跟引擎真实行为一致(不会自相矛盾)。要用 path.sep 拼路径就直接拼, 别再混 '\\'。
export const sep: string = '/';
/** PATH 环境变量的分隔符 */
export const delimiter: string = (__jz.core.platform() === 'win32') ? ';' : ':';

function splitClean(p: any) { return String(p).split(/[\\/]+/).filter(function (s: any) { return s.length; }); }

function collapse(parts: any) {
    const out: any[] = [];
    for (const s of parts) {
        if (s === '.' || s === '') continue;
        if (s === '..') {
            if (out.length && out[out.length - 1] !== '..') out.pop();
            else out.push('..');
        } else out.push(s);
    }
    return out;
}

export function basename(p: any, ext: any) {
    let b = __jz.core.fileName(p);
    if (ext && b.length > ext.length && b.slice(-ext.length) === ext) b = b.slice(0, b.length - ext.length);
    return b;
}

export function dirname(p: any) { return __jz.core.dirName(p); }
export function extname(p: any) { return __jz.core.extName(p); }
export function isAbsolute(p: any) { return __jz.core.isAbsolute(p); }

export function join(...paths: string[]) {
    let absolute = false, first = true;
    const parts: any[] = [];
    for (let i = 0; i < arguments.length; i++) {
        const a = arguments[i];
        if (a === '' || a == null) continue;
        if (first && __jz.core.isAbsolute(a)) absolute = true;
        first = false;
        const segs = splitClean(a);
        for (const s of segs) parts.push(s);
    }
    return (absolute ? '/' : '') + collapse(parts).join('/');
}

export function normalize(p: any) {
    p = String(p);
    const abs = __jz.core.isAbsolute(p);
    const out = collapse(splitClean(p));
    let r = out.join('/');
    if (abs) r = '/' + r;
    return r;
}

export function resolve(...paths: string[]) {
    const parts: any[] = [];
    let absolute = false;
    for (let i = arguments.length - 1; i >= 0; i--) {
        const a = String(arguments[i]);
        if (!a) continue;
        const segs = splitClean(a);
        for (let j = segs.length - 1; j >= 0; j--) parts.unshift(segs[j]);
        if (__jz.core.isAbsolute(a)) { absolute = true; break; }
    }
    if (!absolute) {
        const cwd = splitClean(__jz.core.cwd());
        for (let k = cwd.length - 1; k >= 0; k--) parts.unshift(cwd[k]);
    }
    return (absolute ? '/' : '') + collapse(parts).join('/');
}

export function parse(p: any) {
    p = String(p);
    const base = __jz.core.fileName(p);
    const ext = __jz.core.extName(p);
    const name = ext ? base.slice(0, base.length - ext.length) : base;
    return { root: __jz.core.isAbsolute(p) ? '/' : '', dir: __jz.core.dirName(p), base: base, ext: ext, name: name };
}

export function format(o: any) {
    o = o || {};
    const dir = o.dir || o.root || '';
    const base = o.base || ((o.name || '') + (o.ext || ''));
    if (!dir) return base;
    return dir.replace(/[\\/]+$/, '') + '/' + base;
}

export function relative(a: any, b: any) {
    a = resolve(a); b = resolve(b);
    const as = a.split('/').filter(Boolean), bs = b.split('/').filter(Boolean);
    let i = 0;
    while (i < as.length && i < bs.length && as[i] === bs[i]) i++;
    const rel: any[] = [];
    for (let k = 0; k < as.length - i; k++) rel.push('..');
    for (let k = i; k < bs.length; k++) rel.push(bs[k]);
    return rel.join('/') || '.';
}

const path = { sep, delimiter, basename, dirname, extname, isAbsolute, join, normalize, resolve, parse, format, relative };
export default path;
