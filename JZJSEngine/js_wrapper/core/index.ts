// @jz/core —— fs/path/os/process/crypto 运行时实现(分离式加载)
//
// 对应原 JZJSCoreModule.cpp 内嵌的 g_coreAdapter: 把 __jz.core 原子 API 包装成 Node 风格的
// fs/path/os/process/crypto, 只 export(对齐 Node: 内置模块须 import, 不再挂 globalThis)。
//
// 注意: os 会与 net 模块导出的 os(含 networkInterfaces) 合并。本模块通过下方
// `import { os as netOs }` 显式依赖 net, 由 QuickJS 模块加载器被动解析执行,
// 从而保证 net 先于本模块执行(不依赖入口模块的 import 顺序)。require() 已不再支持。
// 对齐 Node: fs/path/os/process/crypto 都是"内置模块", 只 export, 必须 import 才能用
// (不再挂 globalThis; 入口用 `import { fs, path } from '../jz_js/core/dist/index.js'`)。

// 显式依赖 net: 拿到 net 导出的 os(含 networkInterfaces), 本模块增强它(补 platform/tmpdir)。
import { os as netOs } from 'net/dist';

declare const __jz: any;

function toBytes(data: any, enc: any): Uint8Array {
  if (data === null || data === undefined) return new Uint8Array(0);
  if (typeof data === 'string') {
    if (enc === 'hex') return Uint8Array.fromHex(data);
    if (enc === 'base64') return Uint8Array.fromBase64(data);
    if (enc === 'latin1') {
      const r = new Uint8Array(data.length);
      for (let i = 0; i < data.length; i++) r[i] = data.charCodeAt(i) & 0xff;
      return r;
    }
    return __jz.bytes.fromString(data);
  }
  if (data instanceof Uint8Array) return data;
  if (data instanceof ArrayBuffer) return new Uint8Array(data);
  throw new TypeError('data must be string, Uint8Array or ArrayBuffer');
}
function fromBytes(u8: any, enc: any): string {
  if (!enc || enc === 'utf8' || enc === 'utf-8') return __jz.bytes.toString(u8);
  if (enc === 'hex') return u8.toHex();
  if (enc === 'base64') return u8.toBase64();
  if (enc === 'latin1') { let s = ''; for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]); return s; }
  throw new TypeError('unsupported encoding: ' + enc);
}
function getEncoding(opts: any) {
  if (!opts) return null;
  if (typeof opts === 'string') return opts;
  return opts.encoding || null;
}

function makeStats(s: any) {
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

// Node 风格错误对象: 带上 code/syscall/path(/dest), 供调用方 `catch (e) { e.code === 'ENOENT' }`。
// (底层 __jz.core.* 抛的是原生异常字符串, 没有 code —— 能廉价判定的几类在这里补齐)
function nodeError(code: string, syscall: string, path: any, dest?: any) {
  const e: any = new Error(code + ': ' + syscall + " '" + path + "'" +
    (dest === undefined ? '' : " -> '" + dest + "'"));
  e.code = code;
  e.errno = 0;
  e.syscall = syscall;
  e.path = path;
  if (dest !== undefined) e.dest = dest;
  return e;
}

// 已打开的 fd: 引擎侧(__jz.core.fd*)持有真正的文件句柄, 这里只记编号,
// 用于 Node 语义的 EBADF 判定(引擎侧的 fd 编号从 3 起, 与 Node 保留 0/1/2 一致)。
const openFds = new Set<number>();

function fdOf(fd: any, syscall: string): number {
  const n = Number(fd);
  if (!openFds.has(n)) throw nodeError('EBADF', syscall, n);
  return n;
}

// Node fs 的 flags -> C stdio 模式串(JZFile::open 直接透传给 fopen)。
// 数字 flags 用 fs.constants 里的 O_*(按 Windows CRT 取值); 简化: O_CREAT 走 w 系(会截断)。
function flagsToMode(flags: any): string {
  const c = fs.constants;
  if (typeof flags === 'number') {
    const rw = flags & 3;                       // O_RDONLY=0 / O_WRONLY=1 / O_RDWR=2
    let m = (flags & c.O_APPEND) ? (rw === 2 ? 'a+b' : 'ab')
      : (rw === 0) ? 'rb'
        : (rw === 2) ? 'w+b' : 'wb';
    if ((flags & c.O_EXCL) && (flags & c.O_CREAT)) m += 'x';
    return m;
  }
  const f = (flags === undefined || flags === null) ? 'r' : String(flags);
  const plus = f.indexOf('+') >= 0;
  const head = f.charAt(0);                     // a / w / r(rs 的 O_SYNC 不区分)
  let m = (head === 'a') ? (plus ? 'a+b' : 'ab')
    : (head === 'w') ? (plus ? 'w+b' : 'wb')
      : (plus ? 'r+b' : 'rb');
  if (f.indexOf('x') >= 0 && (head === 'w' || head === 'a')) m += 'x';   // 'x' = O_EXCL
  return m;
}

// ---- fs (Node 同步接口) ----
const fs: any = {
  // Node `fs.constants` 的子集(只列本实现用得到的复制标志与 open flags)
  constants: {
    COPYFILE_EXCL: 1,            // 目标已存在 -> EEXIST
    COPYFILE_FICLONE: 2,         // 本实现不区分(无写时复制, 退化为普通复制)
    COPYFILE_FICLONE_FORCE: 4,
    O_RDONLY: 0,                 // 以下 O_* 取 Windows CRT 值(供 openSync 的数字 flags 用)
    O_WRONLY: 1,
    O_RDWR: 2,
    O_APPEND: 8,
    O_CREAT: 256,
    O_TRUNC: 512,
    O_EXCL: 1024,
    O_SYNC: 0,                   // 不支持, 视为无
    O_NOFOLLOW: 0,               // 不支持(无符号链接概念), 视为无
  },
  existsSync: function (p: any) { return __jz.core.fileExists(p); },
  // Node 允许首参是 fd(path / fd 二选一); fd 形式从**当前游标**读到 EOF。
  readFileSync: function (p: any, opts: any) {
    if (typeof p === 'number') {
      const n = fdOf(p, 'read');
      const parts: Uint8Array[] = [];
      let total = 0;
      for (;;) {
        const chunk = __jz.core.fdRead(n, 64 * 1024);
        if (!chunk.length) break;
        parts.push(chunk);
        total += chunk.length;
      }
      const all = new Uint8Array(total);
      let at = 0;
      for (const c of parts) { all.set(c, at); at += c.length; }
      const enc = getEncoding(opts);
      return enc ? fromBytes(all, enc) : all;
    }
    const u8 = __jz.core.readFile(p);
    const enc = getEncoding(opts);
    return enc ? fromBytes(u8, enc) : u8;
  },
  writeFileSync: function (p: any, data: any, opts: any) {
    const enc = getEncoding(opts);
    if (typeof p === 'number') {
      __jz.core.fdWrite(fdOf(p, 'write'), toBytes(data, enc));   // Node: fd 形式不截断, 从当前游标写
      return;
    }
    __jz.core.writeFile(p, toBytes(data, enc), false);
  },
  appendFileSync: function (p: any, data: any, opts: any) {
    const enc = getEncoding(opts);
    if (typeof p === 'number') {
      __jz.core.fdWrite(fdOf(p, 'write'), toBytes(data, enc));   // Node: fd 形式不做 seek-to-end
      return;
    }
    __jz.core.writeFile(p, toBytes(data, enc), true);
  },
  // Node `fs.openSync(path[, flags[, mode]])` -> fd 编号(从 3 起)。
  // 注: 权限位 mode 在 Windows CRT 下基本无效, 接受但忽略(与 JZFile::open 的能力边界一致)。
  openSync: function (path: any, flags: any, mode: any) {
    const fd = __jz.core.fdOpen(String(path), flagsToMode(flags));
    if (fd < 0) {
      if (!__jz.core.fileExists(path)) throw nodeError('ENOENT', 'open', path);
      throw nodeError('EACCES', 'open', path);
    }
    openFds.add(fd);
    return fd;
  },
  closeSync: function (fd: any) {
    const n = fdOf(fd, 'close');
    if (!__jz.core.fdClose(n)) throw nodeError('EBADF', 'close', n);
    openFds.delete(n);
  },
  // Node `fs.readSync(fd, buffer[, offset[, length[, position]]])` -> 读到的字节数。
  // position 为 null/undefined: 从当前游标读并推进; 为非负整数: 读该偏移且**游标保持不变**。
  readSync: function (fd: any, buffer: any, offset: any, length: any, position: any) {
    const n = fdOf(fd, 'read');
    if (!(buffer instanceof Uint8Array))
      throw new TypeError('ERR_INVALID_ARG_TYPE: buffer must be a Uint8Array');
    const off = (offset === undefined || offset === null) ? 0 : Number(offset);
    const len = (length === undefined || length === null) ? buffer.length - off : Number(length);
    if (off < 0 || len < 0 || off + len > buffer.length)
      throw new RangeError('ERR_OUT_OF_RANGE: offset + length is out of buffer range');

    let saved = -1;
    if (position !== undefined && position !== null) {
      saved = __jz.core.fdTell(n);
      __jz.core.fdSeek(n, Number(position), 0);
    }
    const chunk = __jz.core.fdRead(n, len);        // Uint8Array(最多 len 字节, 空 = EOF)
    for (let i = 0; i < chunk.length; i++) buffer[off + i] = chunk[i];
    if (saved >= 0) __jz.core.fdSeek(n, saved, 0);  // position 形式不改变游标(Node 语义)
    return chunk.length;
  },
  // Node `fs.writeSync(fd, buffer[, offset[, length[, position]]])`
  //   / `fs.writeSync(fd, string[, position[, encoding]])` -> 写入字节数。
  writeSync: function (fd: any, data: any) {
    const n = fdOf(fd, 'write');
    const args = arguments;
    let bytes: Uint8Array;
    let position: any = null;
    if (typeof data === 'string') {
      position = args[2];                                  // writeSync(fd, string, position, encoding)
      bytes = toBytes(data, args[3]);
    } else {
      const buf = toBytes(data, null);
      const off = (args[2] === undefined || args[2] === null) ? 0 : Number(args[2]);
      const len = (args[3] === undefined || args[3] === null) ? buf.length - off : Number(args[3]);
      if (off < 0 || len < 0 || off + len > buf.length)
        throw new RangeError('ERR_OUT_OF_RANGE: offset + length is out of buffer range');
      bytes = buf.subarray(off, off + len);
      position = args[4];
    }

    let saved = -1;
    if (position !== undefined && position !== null) {
      saved = __jz.core.fdTell(n);
      __jz.core.fdSeek(n, Number(position), 0);
    }
    const written = __jz.core.fdWrite(n, bytes);
    if (saved >= 0) __jz.core.fdSeek(n, saved, 0);
    return written;
  },
  // Node `fs.fstatSync(fd)`: 本框架句柄只知道文件大小, 故只有 size() / isFile() 是真实值
  // (时间/权限等字段与 statSync 一样是合成值, 时间为 0)。
  fstatSync: function (fd: any) {
    const n = fdOf(fd, 'fstat');
    const size = __jz.core.fdSize(n);
    if (size < 0) throw nodeError('EBADF', 'fstat', n);
    return makeStats({ isFile: true, isDir: false, size: size, lastModified: 0 });
  },
  unlinkSync: function (p: any) { if (!__jz.core.fileRemove(p)) throw new Error("ENOENT: no such file or directory, unlink '" + p + "'"); },
  // Node `fs.copyFileSync(src, dest[, mode])`: 默认**覆盖**目标; mode 含 COPYFILE_EXCL 时目标
  // 已存在则抛 EEXIST。(底层 JZFile::copy 是分块流式复制, 不整文件读进内存)
  copyFileSync: function (src: any, dest: any, mode: any) {
    const excl = !!(Number(mode || 0) & fs.constants.COPYFILE_EXCL);
    if (!__jz.core.fileExists(src))
      throw nodeError('ENOENT', 'copyfile', src, dest);
    if (excl && __jz.core.fileExists(dest))
      throw nodeError('EEXIST', 'copyfile', src, dest);
    __jz.core.fileCopy(src, dest, !excl);
  },
  // Node `fs.renameSync(oldPath, newPath)`: 改名/移动, 目标已存在时**覆盖**(与 Node 一致)。
  renameSync: function (oldPath: any, newPath: any) {
    if (!__jz.core.fileExists(oldPath))
      throw nodeError('ENOENT', 'rename', oldPath, newPath);
    __jz.core.fileRename(oldPath, newPath);
  },
  // Node `fs.cpSync(src, dest[, opts])`: 文件/目录都能拷; 目录必须 `recursive: true`
  // (缺省 false -> ERR_FS_EISDIR, 与 Node 文档一致); `force` 默认 true(覆盖同名文件)。
  // 注: force=false 时底层撞到同名文件即报错(相当于 Node 的 errorOnExist: true,
  //     本框架暂不做"跳过/重命名"策略)。
  cpSync: function (src: any, dest: any, opts: any) {
    const st = __jz.core.stat(src);
    if (!st) throw nodeError('ENOENT', 'cp', src, dest);
    const recursive = !!(opts && opts.recursive);
    const force = !(opts && opts.force === false);
    if (st.isDir) {
      if (!recursive) throw nodeError('ERR_FS_EISDIR', 'cp', src, dest);
      __jz.core.dirCopy(src, dest, force);
    } else {
      __jz.core.fileCopy(src, dest, force);
    }
  },
  mkdirSync: function (p: any, opts: any) { if (!__jz.core.dirMkdir(p, !!(opts && opts.recursive))) throw new Error("EACCES: mkdir '" + p + "'"); },
  rmdirSync: function (p: any) { if (!__jz.core.dirRmdir(p)) throw new Error("ENOENT: no such file or directory, rmdir '" + p + "'"); },
  rmSync: function (p: any, opts: any) { if (!__jz.core.dirRemove(p, !!(opts && opts.recursive))) throw new Error("ENOENT: no such file or directory, rm '" + p + "'"); },
  readdirSync: function (p: any, opts: any) {
    const list = __jz.core.dirEntryList(p);
    if (!list) throw new Error("ENOENT: no such file or directory, scandir '" + p + "'");
    if (opts && opts.withFileTypes) {
      return list.map(function (e: any) {
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
    return list.map(function (e: any) { return e.name; });
  },
  statSync: function (p: any) {
    const s = __jz.core.stat(p);
    if (!s) throw new Error("ENOENT: no such file or directory, stat '" + p + "'");
    return makeStats(s);
  },
  lstatSync: function (p: any) { return fs.statSync(p); },
};

// ---- path (Node 纯函数接口) ----
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
const path: any = {
  sep: (__jz.core.platform() === 'win32') ? '\\' : '/',
  delimiter: (__jz.core.platform() === 'win32') ? ';' : ':',
  basename: function (p: any, ext: any) {
    let b = __jz.core.fileName(p);
    if (ext && b.length > ext.length && b.slice(-ext.length) === ext) b = b.slice(0, b.length - ext.length);
    return b;
  },
  dirname: function (p: any) { return __jz.core.dirName(p); },
  extname: function (p: any) { return __jz.core.extName(p); },
  isAbsolute: function (p: any) { return __jz.core.isAbsolute(p); },
  join: function () {
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
  },
  normalize: function (p: any) {
    p = String(p);
    const abs = __jz.core.isAbsolute(p);
    const out = collapse(splitClean(p));
    let r = out.join('/');
    if (abs) r = '/' + r;
    return r;
  },
  resolve: function () {
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
  },
  parse: function (p: any) {
    p = String(p);
    const base = __jz.core.fileName(p);
    const ext = __jz.core.extName(p);
    const name = ext ? base.slice(0, base.length - ext.length) : base;
    return { root: __jz.core.isAbsolute(p) ? '/' : '', dir: __jz.core.dirName(p), base: base, ext: ext, name: name };
  },
  format: function (o: any) {
    o = o || {};
    const dir = o.dir || o.root || '';
    const base = o.base || ((o.name || '') + (o.ext || ''));
    if (!dir) return base;
    return dir.replace(/[\\/]+$/, '') + '/' + base;
  },
  relative: function (a: any, b: any) {
    a = path.resolve(a); b = path.resolve(b);
    const as = a.split('/').filter(Boolean), bs = b.split('/').filter(Boolean);
    let i = 0;
    while (i < as.length && i < bs.length && as[i] === bs[i]) i++;
    const rel: any[] = [];
    for (let k = 0; k < as.length - i; k++) rel.push('..');
    for (let k = i; k < bs.length; k++) rel.push(bs[k]);
    return rel.join('/') || '.';
  },
};

// ---- os (增强 net 导出的 os, 保留其 networkInterfaces) ----
const os: any = netOs;
os.platform = function () { return __jz.core.platform(); };
os.tmpdir = function () { return __jz.core.tmpdir(); };
os.EOL = (__jz.core.platform() === 'win32') ? '\r\n' : '\n';

// ---- process ----
const process: any = {
  cwd: function () { return __jz.core.cwd(); },
  execPath: __jz.core.execPath(),
  platform: __jz.core.platform(),
  argv: [],
};

// ---- crypto (Node crypto 子集: randomUUID / randomBytes / randomInt / createHash) ----
const crypto: any = {
  randomUUID: function () { return __jz.core.uuid(); },
  randomBytes: function (size: any, cb: any) {
    const u8 = __jz.core.randomBytes(size);
    if (typeof cb === 'function') { cb(null, u8); return undefined; }
    return u8;
  },
  randomInt: function (min: any, max: any) {
    if (max === undefined) { max = min; min = 0; }
    return __jz.core.randomInt(min, max);
  },
  createHash: function (algorithm: any) {
    algorithm = String(algorithm).toLowerCase();
    if (algorithm !== 'md5')
      throw new Error("ERR_OSSL_EVP_UNSUPPORTED: Unsupported algorithm: " + algorithm);
    let acc = new Uint8Array(0);
    return {
      update: function (data: any, enc: any) {
        const b = toBytes(data, enc);
        const merged = new Uint8Array(acc.length + b.length);
        merged.set(acc, 0);
        merged.set(b, acc.length);
        acc = merged;
        return this;
      },
      digest: function (enc: any) {
        const d = __jz.core.md5(acc);
        return enc ? fromBytes(d, enc) : d;
      },
    };
  },
};

// ---- export(对齐 Node: 内置模块须 import) ----
export { fs, path, os, process, crypto };

// bytes: 二进制读写工具(ByteReader/ByteWriter/...), **不是** Node 内置模块, 是本框架
// 补的那层"游标自增"糖。也允许按子路径直接 import:
//     import { ByteReader } from '<...>/jz_js/core/dist/bytes.js';
// 走子路径不会执行本文件, 也就不需要 __jz.core 存在(协议解析器可以只依赖它)。
export { ByteReader, ByteWriter, concatBytes, hexOf } from './bytes';
export type { Bytes } from './bytes';
