#include "JZJSCoreModule.h"

#include <cstdio>
#include <random>

#include "JZJSBind.h"
#include "JZUuid.h"
#include "JZCrypto.h"

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

// ---- crypto / 随机 / UUID ----
std::string JZJSCoreModule::uuid()
{
    return JZUuid::createUuid().toString(JZUuid::WithoutBraces);
}

JSValue JZJSCoreModule::randomBytes(int size)
{
    if (size < 0)
        return throwError("ERR_OUT_OF_RANGE: size must be >= 0");
    JZByteArray out;
    if (size > 0)
    {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dist(0, 255);
        out.resize(size);
        for (int i = 0; i < size; i++)
            out[i] = static_cast<char>(dist(gen));
    }
    return makeUint8Array(out);
}

JSValue JZJSCoreModule::randomInt(int min, int max)
{
    if (max <= min)
        return JS_ThrowRangeError(m_engine->context(),
            "ERR_OUT_OF_RANGE: max must be greater than min");
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(min, max - 1);
    return JS_NewInt32(m_engine->context(), dist(gen));
}

JSValue JZJSCoreModule::md5(JSValue data)
{
    JZByteArray buf;
    if (!jsValueToBytes(m_engine->context(), data, buf))
        return throwError("ERR_INVALID_ARG_TYPE: data must be string, Uint8Array or ArrayBuffer");
    JZByteArray digest = JZCrypto::md5(buf);
    return makeUint8Array(digest);
}

// ======================================================================
// JS 适配层: 把原子封装成 Node 风格 fs/path/os/process/crypto
// ======================================================================

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
    m_engine->registFunction(ns, "uuid", jsbind::createSelfFunction(this, &JZJSCoreModule::uuid));
    m_engine->registFunction(ns, "randomBytes", jsbind::createSelfFunction(this, &JZJSCoreModule::randomBytes));
    m_engine->registFunction(ns, "randomInt", jsbind::createSelfFunction(this, &JZJSCoreModule::randomInt));
    m_engine->registFunction(ns, "md5", jsbind::createSelfFunction(this, &JZJSCoreModule::md5));
    m_engine->setProperty(jz, "core", ns);

    if (jzFromGet)
        JS_FreeValue(m_engine->context(), jz);   // 释放 getProperty 多 dup 的引用
}

void JZJSCoreModule::deinitEngine()
{
    m_engine = nullptr;
}
