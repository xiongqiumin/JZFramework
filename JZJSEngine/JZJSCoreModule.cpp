#include "JZJSCoreModule.h"

#include <cstdio>

#include "JZJSBind.h"

JZJSCoreModule::JZJSCoreModule()
{
}

JZJSCoreModule::~JZJSCoreModule()
{
}

// ======================================================================
// 工具
// ======================================================================
JSValue JZJSCoreModule::makeUint8Array(const JZByteArray& data)
{
    return JS_NewUint8ArrayCopy(m_engine->context(), (const uint8_t*)data.data(), data.size());
}

JSValue JZJSCoreModule::throwError(const std::string& msg)
{
    JSContext* ctx = m_engine->context();
    return JS_ThrowTypeError(ctx, "%s", msg.c_str());
}

static std::string jsValueToString(JSContext* ctx, JSValue v)
{
    const char* s = JS_ToCString(ctx, v);
    if (!s)
        return std::string();
    std::string r(s);
    JS_FreeCString(ctx, s);
    return r;
}

bool JZJSCoreModule::jsValueToBytes(JSContext* ctx, JSValue v, JZByteArray& out)
{
    if (JS_IsString(v))
    {
        JZByteArray data = str_toUtf8(jsValueToString(ctx, v));
        out = data;
        return true;
    }

    size_t size = 0;
    uint8_t* buf = JS_GetUint8Array(ctx, &size, v);
    if (buf)
    {
        out = JZByteArray((const char*)buf, (int)size);
        return true;
    }

    if (JS_IsArrayBuffer(v))
    {
        size_t ab_size = 0;
        uint8_t* ab = JS_GetArrayBuffer(ctx, &ab_size, v);
        if (ab)
        {
            out = JZByteArray((const char*)ab, (int)ab_size);
            return true;
        }
    }

    return false;
}

// ======================================================================
// 原子 API: 直接映射到 JZCore 的同步实现
// ======================================================================
bool JZJSCoreModule::fileExists(const std::string& path)
{
    return JZFileInfo::exists(path);
}

bool JZJSCoreModule::fileRemove(const std::string& path)
{
    return JZFile::remove(path);
}

JSValue JZJSCoreModule::readFile(const std::string& path)
{
    JZFile f;
    if (!f.open(path, "rb"))
        return throwError("ENOENT: no such file or directory, open '" + path + "'");
    JZByteArray data = f.readAll();
    f.close();
    return makeUint8Array(data);
}

JSValue JZJSCoreModule::writeFile(const std::string& path, JSValue data, bool append)
{
    JZByteArray buf;
    if (!jsValueToBytes(m_engine->context(), data, buf))
        return throwError("ERR_INVALID_ARG_TYPE: data must be string, Uint8Array or ArrayBuffer");
    JZFile f;
    if (!f.open(path, append ? "ab" : "wb"))
        return throwError("EACCES: unable to open file for writing '" + path + "'");
    f.write(buf);
    f.close();
    return JS_UNDEFINED;
}

Json::Value JZJSCoreModule::stat(const std::string& path)
{
    JZFileInfo fi(path);
    if (!fi.exists())
        return Json::Value();
    Json::Value v;
    v["isFile"] = fi.isFile();
    v["isDir"] = fi.isDir();
    v["size"] = fi.size();
    v["lastModified"] = (Json::Int64)fi.lastModified();
    return v;
}

Json::Value JZJSCoreModule::dirEntryList(const std::string& path)
{
    JZDir d(path);
    if (!d.exists())
        return Json::Value();
    std::vector<JZFileInfo> list = d.entryInfoList(JZDir::Files | JZDir::Dirs | JZDir::NoDotAndDotDot);
    Json::Value arr(Json::arrayValue);
    for (auto& fi : list)
    {
        Json::Value e;
        e["name"] = fi.fileName();
        e["isFile"] = fi.isFile();
        e["isDir"] = fi.isDir();
        e["size"] = fi.size();
        e["lastModified"] = (Json::Int64)fi.lastModified();
        arr.append(e);
    }
    return arr;
}

bool JZJSCoreModule::dirMkdir(const std::string& path, bool recursive)
{
    JZDir d;
    return recursive ? d.mkpath(path) : d.mkdir(path);
}

bool JZJSCoreModule::dirRmdir(const std::string& path)
{
    JZDir d;
    return d.rmdir(path);
}

bool JZJSCoreModule::dirRemove(const std::string& path, bool recursive)
{
    JZFileInfo fi(path);
    JZDir d(path);
    if (fi.exists() && fi.isDir())
    {
        if (recursive)
            return d.removeRecursively();
        return d.rmdir(path);
    }
    return JZFile::remove(path);
}

std::string JZJSCoreModule::fileName(const std::string& path)
{
    return JZFileInfo(path).fileName();
}

std::string JZJSCoreModule::dirName(const std::string& path)
{
    return JZFileInfo(path).path();
}

std::string JZJSCoreModule::extName(const std::string& path)
{
    std::string s = JZFileInfo(path).suffix();
    return s.empty() ? std::string() : ("." + s);
}

bool JZJSCoreModule::isAbsolute(const std::string& path)
{
    return JZDir::isAbsolutePath(path);
}

std::string JZJSCoreModule::platform()
{
    std::string p = JZPlatform::platform();
    if (p == "windows")
        return "win32";
    return p;
}

std::string JZJSCoreModule::tmpdir()
{
    return JZPlatform::applicationTmpPath();
}

std::string JZJSCoreModule::cwd()
{
    return JZDir::currentPath();
}

std::string JZJSCoreModule::execPath()
{
    return JZPlatform::applicationFilePath();
}

// ======================================================================
// JS 适配层: 把原子封装成 Node 风格 fs/path/os/process
// ======================================================================
static const char* g_coreAdapter = R"(
(function () {
  const core = __jz.core;

  function toBytes(data, enc) {
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
  function fromBytes(u8, enc) {
    if (!enc || enc === 'utf8' || enc === 'utf-8') return __jz.bytes.toString(u8);
    if (enc === 'hex') return u8.toHex();
    if (enc === 'base64') return u8.toBase64();
    if (enc === 'latin1') { let s = ''; for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]); return s; }
    throw new TypeError('unsupported encoding: ' + enc);
  }
  function getEncoding(opts) {
    if (!opts) return null;
    if (typeof opts === 'string') return opts;
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
    existsSync: function (p) { return core.fileExists(p); },
    readFileSync: function (p, opts) {
      const u8 = core.readFile(p);
      const enc = getEncoding(opts);
      return enc ? fromBytes(u8, enc) : u8;
    },
    writeFileSync: function (p, data, opts) {
      const enc = getEncoding(opts);
      core.writeFile(p, toBytes(data, enc), false);
    },
    appendFileSync: function (p, data, opts) {
      const enc = getEncoding(opts);
      core.writeFile(p, toBytes(data, enc), true);
    },
    unlinkSync: function (p) { if (!core.fileRemove(p)) throw new Error("ENOENT: no such file or directory, unlink '" + p + "'"); },
    mkdirSync: function (p, opts) { if (!core.dirMkdir(p, !!(opts && opts.recursive))) throw new Error("EACCES: mkdir '" + p + "'"); },
    rmdirSync: function (p) { if (!core.dirRmdir(p)) throw new Error("ENOENT: no such file or directory, rmdir '" + p + "'"); },
    rmSync: function (p, opts) { if (!core.dirRemove(p, !!(opts && opts.recursive))) throw new Error("ENOENT: no such file or directory, rm '" + p + "'"); },
    readdirSync: function (p, opts) {
      const list = core.dirEntryList(p);
      if (!list) throw new Error("ENOENT: no such file or directory, scandir '" + p + "'");
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
      const s = core.stat(p);
      if (!s) throw new Error("ENOENT: no such file or directory, stat '" + p + "'");
      return makeStats(s);
    },
    lstatSync: function (p) { return fs.statSync(p); },
  };

  // ---- path (Node 纯函数接口) ----
  function splitClean(p) { return String(p).split(/[\\/]+/).filter(function (s) { return s.length; }); }
  function collapse(parts) {
    const out = [];
    for (const s of parts) {
      if (s === '.' || s === '') continue;
      if (s === '..') {
        if (out.length && out[out.length - 1] !== '..') out.pop();
        else out.push('..');
      } else out.push(s);
    }
    return out;
  }
  const path = {
    sep: (core.platform() === 'win32') ? '\\' : '/',
    delimiter: (core.platform() === 'win32') ? ';' : ':',
    basename: function (p, ext) {
      let b = core.fileName(p);
      if (ext && b.length > ext.length && b.slice(-ext.length) === ext) b = b.slice(0, b.length - ext.length);
      return b;
    },
    dirname: function (p) { return core.dirName(p); },
    extname: function (p) { return core.extName(p); },
    isAbsolute: function (p) { return core.isAbsolute(p); },
    join: function () {
      let absolute = false, first = true;
      const parts = [];
      for (let i = 0; i < arguments.length; i++) {
        const a = arguments[i];
        if (a === '' || a == null) continue;
        if (first && core.isAbsolute(a)) absolute = true;
        first = false;
        const segs = splitClean(a);
        for (const s of segs) parts.push(s);
      }
      return (absolute ? '/' : '') + collapse(parts).join('/');
    },
    normalize: function (p) {
      p = String(p);
      const abs = core.isAbsolute(p);
      const out = collapse(splitClean(p));
      let r = out.join('/');
      if (abs) r = '/' + r;
      return r;
    },
    resolve: function () {
      const parts = [];
      let absolute = false;
      for (let i = arguments.length - 1; i >= 0; i--) {
        const a = String(arguments[i]);
        if (!a) continue;
        const segs = splitClean(a);
        for (let j = segs.length - 1; j >= 0; j--) parts.unshift(segs[j]);
        if (core.isAbsolute(a)) { absolute = true; break; }
      }
      if (!absolute) {
        const cwd = splitClean(core.cwd());
        for (let k = cwd.length - 1; k >= 0; k--) parts.unshift(cwd[k]);
      }
      return (absolute ? '/' : '') + collapse(parts).join('/');
    },
    parse: function (p) {
      p = String(p);
      const base = core.fileName(p);
      const ext = core.extName(p);
      const name = ext ? base.slice(0, base.length - ext.length) : base;
      return { root: core.isAbsolute(p) ? '/' : '', dir: core.dirName(p), base: base, ext: ext, name: name };
    },
    format: function (o) {
      o = o || {};
      const dir = o.dir || o.root || '';
      const base = o.base || ((o.name || '') + (o.ext || ''));
      if (!dir) return base;
      return dir.replace(/[\\/]+$/, '') + '/' + base;
    },
    relative: function (a, b) {
      a = path.resolve(a); b = path.resolve(b);
      const as = a.split('/').filter(Boolean), bs = b.split('/').filter(Boolean);
      let i = 0;
      while (i < as.length && i < bs.length && as[i] === bs[i]) i++;
      const rel = [];
      for (let k = 0; k < as.length - i; k++) rel.push('..');
      for (let k = i; k < bs.length; k++) rel.push(bs[k]);
      return rel.join('/') || '.';
    },
  };

  // ---- os (增强 net 模块已注入的全局, 保留其 networkInterfaces) ----
  const os = globalThis.os || {};
  os.platform = function () { return core.platform(); };
  os.tmpdir = function () { return core.tmpdir(); };
  os.EOL = (core.platform() === 'win32') ? '\r\n' : '\n';
  globalThis.os = os;

  // ---- process ----
  const process = {
    cwd: function () { return core.cwd(); },
    execPath: core.execPath(),
    platform: core.platform(),
    argv: [],
  };
  globalThis.process = process;

  // ---- 全局 + require ----
  globalThis.fs = fs;
  globalThis.path = path;

  const origRequire = globalThis.require;
  globalThis.require = function (name) {
    if (name === 'fs') return fs;
    if (name === 'path') return path;
    if (name === 'process') return process;
    if (name === 'os') return os;
    return origRequire ? origRequire(name) : undefined;
  };
})();
)";

// ======================================================================
// 引擎注册
// ======================================================================
void JZJSCoreModule::initEngine(JZJSEngine* engine)
{
    m_engine = engine;
    auto global = m_engine->globalObject();

    // 复用 net 模块已建的 __jz(不要覆盖其下的 net/dgram/... 子命名空间)。
    // getProperty 返回的是多 dup 的一份引用, 用完后必须释放, 否则 __jz 在
    // JS_FreeRuntime 时仍被 C++ 持有 -> gc_obj_list 非空触发断言。
    JSValue jz = m_engine->getProperty(global, "__jz");
    bool jzFromGet = !JS_IsUndefined(jz);
    if (!jzFromGet)
    {
        jz = m_engine->createObject();
        m_engine->setProperty(global, "__jz", jz);
    }

    JSValue ns = m_engine->createObject();
    m_engine->registFunction(ns, "fileExists", jsbind::createSelfFunction(this, &JZJSCoreModule::fileExists));
    m_engine->registFunction(ns, "fileRemove", jsbind::createSelfFunction(this, &JZJSCoreModule::fileRemove));
    m_engine->registFunction(ns, "readFile", jsbind::createSelfFunction(this, &JZJSCoreModule::readFile));
    m_engine->registFunction(ns, "writeFile", jsbind::createSelfFunction(this, &JZJSCoreModule::writeFile));
    m_engine->registFunction(ns, "stat", jsbind::createSelfFunction(this, &JZJSCoreModule::stat));
    m_engine->registFunction(ns, "dirEntryList", jsbind::createSelfFunction(this, &JZJSCoreModule::dirEntryList));
    m_engine->registFunction(ns, "dirMkdir", jsbind::createSelfFunction(this, &JZJSCoreModule::dirMkdir));
    m_engine->registFunction(ns, "dirRmdir", jsbind::createSelfFunction(this, &JZJSCoreModule::dirRmdir));
    m_engine->registFunction(ns, "dirRemove", jsbind::createSelfFunction(this, &JZJSCoreModule::dirRemove));
    m_engine->registFunction(ns, "fileName", jsbind::createSelfFunction(this, &JZJSCoreModule::fileName));
    m_engine->registFunction(ns, "dirName", jsbind::createSelfFunction(this, &JZJSCoreModule::dirName));
    m_engine->registFunction(ns, "extName", jsbind::createSelfFunction(this, &JZJSCoreModule::extName));
    m_engine->registFunction(ns, "isAbsolute", jsbind::createSelfFunction(this, &JZJSCoreModule::isAbsolute));
    m_engine->registFunction(ns, "platform", jsbind::createSelfFunction(this, &JZJSCoreModule::platform));
    m_engine->registFunction(ns, "tmpdir", jsbind::createSelfFunction(this, &JZJSCoreModule::tmpdir));
    m_engine->registFunction(ns, "cwd", jsbind::createSelfFunction(this, &JZJSCoreModule::cwd));
    m_engine->registFunction(ns, "execPath", jsbind::createSelfFunction(this, &JZJSCoreModule::execPath));
    m_engine->setProperty(jz, "core", ns);

    if (jzFromGet)
        JS_FreeValue(m_engine->context(), jz);   // 释放 getProperty 多 dup 的引用

    m_engine->eval(g_coreAdapter);
}

void JZJSCoreModule::deinitEngine()
{
    m_engine = nullptr;
}
