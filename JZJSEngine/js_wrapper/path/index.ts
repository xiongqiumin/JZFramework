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

// Windows 盘符: `C:/a` 的第一段是 "C:"。它必须当**前缀**保留, 不能像普通段那样参与 '/' 拼接 ——
// 否则 join("C:/a","b") 得到 "/C:/a/b"(多一个前导斜杠), fs 打不开(Node 里是 "C:/a/b")。
const kDrive = /^([A-Za-z]:)[\\/]/;

function splitParts(p: any) {
    const s = String(p);
    const m = kDrive.exec(s);
    const drive = m ? m[1] : '';
    return { drive: drive, segs: splitClean(drive ? s.slice(drive.length) : s) };
}

/** 绝对路径的前缀: 有盘符用 "C:/", 没有就用 "/"(MSYS 风格的 /c/tmp 之类) */
function prefixOf(drive: string, absolute: boolean) {
    if (drive) return drive + '/';
    return absolute ? '/' : '';
}

// absolute: 绝对路径的 `..` 到根就停(Node 同: 不会冒出 "C:/../x", 只会是 "C:/x");
// 相对路径才保留前导 `..`。
function collapse(parts: any, absolute?: boolean) {
    const out: any[] = [];
    for (const s of parts) {
        if (s === '.' || s === '') continue;
        if (s === '..') {
            if (out.length && out[out.length - 1] !== '..') out.pop();
            else if (!absolute) out.push('..');
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
    let absolute = false, drive = '', first = true;
    const parts: any[] = [];
    for (let i = 0; i < arguments.length; i++) {
        const a = arguments[i];
        if (a === '' || a == null) continue;
        const sp = splitParts(a);
        if (first && __jz.core.isAbsolute(a)) { absolute = true; drive = sp.drive; }
        first = false;
        for (const s of sp.segs) parts.push(s);
    }
    return prefixOf(drive, absolute) + collapse(parts, absolute || !!drive).join('/');
}

export function normalize(p: any) {
    p = String(p);
    const sp = splitParts(p);
    const abs = __jz.core.isAbsolute(p);
    let r = prefixOf(sp.drive, abs) + collapse(sp.segs, abs || !!sp.drive).join('/');
    // Node 的 normalize 保留结尾分隔符("C:/a//b/" -> "C:/a/b/")
    if (r && /[\\/]$/.test(p) && r.charAt(r.length - 1) !== '/') r += '/';
    return r;
}

// ⚠ 与 Node(win32) 的**有意差异**: resolve("/c/tmp","x")
//   Node 把 "/c/tmp" 当"当前盘的根相对路径", 给 "C:/c/tmp/x";
//   本运行时里 "/c/..." 是 MSYS 风格的真实路径(codeBashPathToWindows 会翻成 "C:/..."),
//   fs 也按绝对路径处理, 所以这里给 "/c/tmp/x" —— 跟引擎其它部分保持一致, 不学 Node 那种怪行为。
export function resolve(...paths: string[]) {
    const parts: any[] = [];
    let absolute = false, drive = '';
    for (let i = arguments.length - 1; i >= 0; i--) {
        const a = String(arguments[i]);
        if (!a) continue;
        const sp = splitParts(a);
        for (let j = sp.segs.length - 1; j >= 0; j--) parts.unshift(sp.segs[j]);
        if (__jz.core.isAbsolute(a)) { absolute = true; drive = sp.drive; break; }
    }
    if (!absolute) {
        // 没有绝对路径参数 -> 相对当前工作目录解析(Node 同): 盘符也跟着 cwd 走
        const cwd = splitParts(__jz.core.cwd());
        drive = cwd.drive;
        for (let k = cwd.segs.length - 1; k >= 0; k--) parts.unshift(cwd.segs[k]);
    }
    return prefixOf(drive, absolute || !!drive) + collapse(parts, absolute || !!drive).join('/');
}

export function parse(p: any) {
    p = String(p);
    const base = __jz.core.fileName(p);
    const ext = __jz.core.extName(p);
    const name = ext ? base.slice(0, base.length - ext.length) : base;
    // Node: parse("C:/a/b.txt").root === "C:/"(不是 "/")
    const dm = kDrive.exec(p);
    const root = dm ? dm[1] + '/' : (__jz.core.isAbsolute(p) ? '/' : '');
    return { root: root, dir: __jz.core.dirName(p), base: base, ext: ext, name: name };
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
    // Node: 同一个路径 -> 空串(以前返回 ".", 会让 `rel || '.'` 这类写法行为不一致)
    return rel.join('/');
}

const path = { sep, delimiter, basename, dirname, extname, isAbsolute, join, normalize, resolve, parse, format, relative };
export default path;
