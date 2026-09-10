// @jz/core —— fs/path/os/process/crypto 运行时实现(分离式加载)
//
// 对应原 JZJSCoreModule.cpp 内嵌的 g_coreAdapter: 把 __jz.core 原子 API 包装成 Node 风格的
// fs/path/os/process/crypto, 挂到 globalThis, 并扩展 require() 以识别这些模块。
//
// 注意: os 会与 net 模块注入的全局 os(含 networkInterfaces) 合并, 故本模块须在 net 之后加载。
function toBytes(data, enc) {
    if (data === null || data === undefined)
        return new Uint8Array(0);
    if (typeof data === 'string') {
        if (enc === 'hex')
            return Uint8Array.fromHex(data);
        if (enc === 'base64')
            return Uint8Array.fromBase64(data);
        if (enc === 'latin1') {
            const r = new Uint8Array(data.length);
            for (let i = 0; i < data.length; i++)
                r[i] = data.charCodeAt(i) & 0xff;
            return r;
        }
        return __jz.bytes.fromString(data);
    }
    if (data instanceof Uint8Array)
        return data;
    if (data instanceof ArrayBuffer)
        return new Uint8Array(data);
    throw new TypeError('data must be string, Uint8Array or ArrayBuffer');
}
function fromBytes(u8, enc) {
    if (!enc || enc === 'utf8' || enc === 'utf-8')
        return __jz.bytes.toString(u8);
    if (enc === 'hex')
        return u8.toHex();
    if (enc === 'base64')
        return u8.toBase64();
    if (enc === 'latin1') {
        let s = '';
        for (let i = 0; i < u8.length; i++)
            s += String.fromCharCode(u8[i]);
        return s;
    }
    throw new TypeError('unsupported encoding: ' + enc);
}
function getEncoding(opts) {
    if (!opts)
        return null;
    if (typeof opts === 'string')
        return opts;
    return opts.encoding || null;
}
function makeStats(s) {
    const ms = s.lastModified;
    return {
        dev: 0, mode: s.isDir ? 0o777 : 0o666, nlink: 1, uid: 0, gid: 0, rdev: 0,
        blksize: 0, ino: 0, size: s.size, blocks: 0,
        atimeMs: ms, mtimeMs: ms, ctimeMs: ms, birthtimeMs: ms,
        atime: new Date(ms), mtime: new Date(ms), ctime: new Date(ms), birthtime: new Date(ms),
        isFile: function () { return s.isFile; },
        isDirectory: function () { return s.isDir; },
        isBlockDevice: function () { return false; },
        isCharacterDevice: function () { return false; },
        isSymbolicLink: function () { return false; },
        isFIFO: function () { return false; },
        isSocket: function () { return false; },
    };
}
// ---- fs (Node 同步接口) ----
const fs = {
    existsSync: function (p) { return __jz.core.fileExists(p); },
    readFileSync: function (p, opts) {
        const u8 = __jz.core.readFile(p);
        const enc = getEncoding(opts);
        return enc ? fromBytes(u8, enc) : u8;
    },
    writeFileSync: function (p, data, opts) {
        const enc = getEncoding(opts);
        __jz.core.writeFile(p, toBytes(data, enc), false);
    },
    appendFileSync: function (p, data, opts) {
        const enc = getEncoding(opts);
        __jz.core.writeFile(p, toBytes(data, enc), true);
    },
    unlinkSync: function (p) { if (!__jz.core.fileRemove(p))
        throw new Error("ENOENT: no such file or directory, unlink '" + p + "'"); },
    mkdirSync: function (p, opts) { if (!__jz.core.dirMkdir(p, !!(opts && opts.recursive)))
        throw new Error("EACCES: mkdir '" + p + "'"); },
    rmdirSync: function (p) { if (!__jz.core.dirRmdir(p))
        throw new Error("ENOENT: no such file or directory, rmdir '" + p + "'"); },
    rmSync: function (p, opts) { if (!__jz.core.dirRemove(p, !!(opts && opts.recursive)))
        throw new Error("ENOENT: no such file or directory, rm '" + p + "'"); },
    readdirSync: function (p, opts) {
        const list = __jz.core.dirEntryList(p);
        if (!list)
            throw new Error("ENOENT: no such file or directory, scandir '" + p + "'");
        if (opts && opts.withFileTypes) {
            return list.map(function (e) {
                return {
                    name: e.name,
                    isFile: function () { return e.isFile; },
                    isDirectory: function () { return e.isDir; },
                    isBlockDevice: function () { return false; },
                    isCharacterDevice: function () { return false; },
                    isSymbolicLink: function () { return false; },
                    isFIFO: function () { return false; },
                    isSocket: function () { return false; },
                };
            });
        }
        return list.map(function (e) { return e.name; });
    },
    statSync: function (p) {
        const s = __jz.core.stat(p);
        if (!s)
            throw new Error("ENOENT: no such file or directory, stat '" + p + "'");
        return makeStats(s);
    },
    lstatSync: function (p) { return fs.statSync(p); },
};
// ---- path (Node 纯函数接口) ----
function splitClean(p) { return String(p).split(/[\\/]+/).filter(function (s) { return s.length; }); }
function collapse(parts) {
    const out = [];
    for (const s of parts) {
        if (s === '.' || s === '')
            continue;
        if (s === '..') {
            if (out.length && out[out.length - 1] !== '..')
                out.pop();
            else
                out.push('..');
        }
        else
            out.push(s);
    }
    return out;
}
const path = {
    sep: (__jz.core.platform() === 'win32') ? '\\' : '/',
    delimiter: (__jz.core.platform() === 'win32') ? ';' : ':',
    basename: function (p, ext) {
        let b = __jz.core.fileName(p);
        if (ext && b.length > ext.length && b.slice(-ext.length) === ext)
            b = b.slice(0, b.length - ext.length);
        return b;
    },
    dirname: function (p) { return __jz.core.dirName(p); },
    extname: function (p) { return __jz.core.extName(p); },
    isAbsolute: function (p) { return __jz.core.isAbsolute(p); },
    join: function () {
        let absolute = false, first = true;
        const parts = [];
        for (let i = 0; i < arguments.length; i++) {
            const a = arguments[i];
            if (a === '' || a == null)
                continue;
            if (first && __jz.core.isAbsolute(a))
                absolute = true;
            first = false;
            const segs = splitClean(a);
            for (const s of segs)
                parts.push(s);
        }
        return (absolute ? '/' : '') + collapse(parts).join('/');
    },
    normalize: function (p) {
        p = String(p);
        const abs = __jz.core.isAbsolute(p);
        const out = collapse(splitClean(p));
        let r = out.join('/');
        if (abs)
            r = '/' + r;
        return r;
    },
    resolve: function () {
        const parts = [];
        let absolute = false;
        for (let i = arguments.length - 1; i >= 0; i--) {
            const a = String(arguments[i]);
            if (!a)
                continue;
            const segs = splitClean(a);
            for (let j = segs.length - 1; j >= 0; j--)
                parts.unshift(segs[j]);
            if (__jz.core.isAbsolute(a)) {
                absolute = true;
                break;
            }
        }
        if (!absolute) {
            const cwd = splitClean(__jz.core.cwd());
            for (let k = cwd.length - 1; k >= 0; k--)
                parts.unshift(cwd[k]);
        }
        return (absolute ? '/' : '') + collapse(parts).join('/');
    },
    parse: function (p) {
        p = String(p);
        const base = __jz.core.fileName(p);
        const ext = __jz.core.extName(p);
        const name = ext ? base.slice(0, base.length - ext.length) : base;
        return { root: __jz.core.isAbsolute(p) ? '/' : '', dir: __jz.core.dirName(p), base: base, ext: ext, name: name };
    },
    format: function (o) {
        o = o || {};
        const dir = o.dir || o.root || '';
        const base = o.base || ((o.name || '') + (o.ext || ''));
        if (!dir)
            return base;
        return dir.replace(/[\\/]+$/, '') + '/' + base;
    },
    relative: function (a, b) {
        a = path.resolve(a);
        b = path.resolve(b);
        const as = a.split('/').filter(Boolean), bs = b.split('/').filter(Boolean);
        let i = 0;
        while (i < as.length && i < bs.length && as[i] === bs[i])
            i++;
        const rel = [];
        for (let k = 0; k < as.length - i; k++)
            rel.push('..');
        for (let k = i; k < bs.length; k++)
            rel.push(bs[k]);
        return rel.join('/') || '.';
    },
};
// ---- os (增强 net 模块已注入的全局, 保留其 networkInterfaces) ----
const os = globalThis.os || {};
os.platform = function () { return __jz.core.platform(); };
os.tmpdir = function () { return __jz.core.tmpdir(); };
os.EOL = (__jz.core.platform() === 'win32') ? '\r\n' : '\n';
globalThis.os = os;
// ---- process ----
const process = {
    cwd: function () { return __jz.core.cwd(); },
    execPath: __jz.core.execPath(),
    platform: __jz.core.platform(),
    argv: [],
};
globalThis.process = process;
// ---- crypto (Node crypto 子集: randomUUID / randomBytes / randomInt / createHash) ----
const crypto = {
    randomUUID: function () { return __jz.core.uuid(); },
    randomBytes: function (size, cb) {
        const u8 = __jz.core.randomBytes(size);
        if (typeof cb === 'function') {
            cb(null, u8);
            return undefined;
        }
        return u8;
    },
    randomInt: function (min, max) {
        if (max === undefined) {
            max = min;
            min = 0;
        }
        return __jz.core.randomInt(min, max);
    },
    createHash: function (algorithm) {
        algorithm = String(algorithm).toLowerCase();
        if (algorithm !== 'md5')
            throw new Error("ERR_OSSL_EVP_UNSUPPORTED: Unsupported algorithm: " + algorithm);
        let acc = new Uint8Array(0);
        return {
            update: function (data, enc) {
                const b = toBytes(data, enc);
                const merged = new Uint8Array(acc.length + b.length);
                merged.set(acc, 0);
                merged.set(b, acc.length);
                acc = merged;
                return this;
            },
            digest: function (enc) {
                const d = __jz.core.md5(acc);
                return enc ? fromBytes(d, enc) : d;
            },
        };
    },
};
globalThis.crypto = crypto;
// ---- 全局(供全局脚本直接访问; import 用法由 export 提供) ----
globalThis.fs = fs;
globalThis.path = path;
export { fs, path, os, process, crypto };
