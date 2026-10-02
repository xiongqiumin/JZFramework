#include "JZJSAudioModule.h"

#include <algorithm>
#include <string>

#include "JZDebug.h"
#include "JZJSBind.h"
#include "JZJSHandleRef.h"

#ifdef HAS_JZAudio
// ============================================================================
// 编入 JZAudio 的版本
// ============================================================================
#include "JZAudio/JZAudioInput.h"
#include "JZAudio/JZAudioOutput.h"
#include "JZAudio/JZAudioDeviceInfo.h"
#include "JZAudio/JZAudioFormat.h"
#include "JZAudio/JZWaveFile.h"
#include "JZAudio/JZAudioUtils.h"

namespace
{
// 采集缓冲上限: 8MB ≈ 16kHz/16bit/单声道 约 4 分钟。JS 一直不取就丢最老的(并置 overrun)。
const int kMaxBuffered = 8 * 1024 * 1024;

JSValue jsMakeUint8Array(JSContext* ctx, const JZByteArray& data)
{
    return JS_NewUint8ArrayCopy(ctx, (const uint8_t*)data.data(), (size_t)data.size());
}

// Uint8Array/Buffer/ArrayBuffer/字符串 -> JZByteArray（与 JZJSSerialPortModule 同款, 便于互认）
bool jsValueToBytes(JSContext* ctx, JSValue v, JZByteArray& out)
{
    out.clear();
    if (JS_IsString(v))
    {
        size_t len = 0;
        const char* p = JS_ToCStringLen(ctx, &len, v);
        if (!p)
            return false;
        out = JZByteArray(p, (int)len);
        JS_FreeCString(ctx, p);
        return true;
    }

    size_t size = 0;
    uint8_t* buf = jzUint8ArrayOrNull(ctx, &size, v);
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

std::string strOf(const Json::Value& v, const char* k, const char* def)
{
    if (v.isMember(k) && v[k].isString())
        return v[k].asString();
    return std::string(def);
}

int intOf(const Json::Value& v, const char* k, int def)
{
    if (v.isMember(k) && v[k].isNumeric())
        return v[k].asInt();
    return def;
}

// 采样格式: 收 'int16'/'uint8'/'int32'/'float'（大小写随意）或 JZAudioFormat::SampleFormat 的整数
JZAudioFormat::SampleFormat sampleFormatOf(const Json::Value& v, JZAudioFormat::SampleFormat def)
{
    if (!v.isMember("sampleFormat"))
        return def;
    const Json::Value& s = v["sampleFormat"];
    if (s.isNumeric())
    {
        const int n = s.asInt();
        if (n >= (int)JZAudioFormat::UInt8 && n <= (int)JZAudioFormat::Float)
            return (JZAudioFormat::SampleFormat)n;
        return def;
    }
    if (!s.isString())
        return def;
    std::string k = s.asString();
    std::transform(k.begin(), k.end(), k.begin(), ::tolower);
    if (k == "uint8" || k == "u8")   return JZAudioFormat::UInt8;
    if (k == "int16" || k == "i16")  return JZAudioFormat::Int16;
    if (k == "int32" || k == "i32")  return JZAudioFormat::Int32;
    if (k == "float" || k == "f32")  return JZAudioFormat::Float;
    return def;
}

JZAudioFormat fmtOf(const Json::Value& v, const JZAudioFormat& def)
{
    JZAudioFormat f = def;                      // 默认: 1 声道 / 16000Hz / Int16（见 JZAudioFormat 构造）
    f.sampleRate = intOf(v, "sampleRate", f.sampleRate);
    f.channelCount = intOf(v, "channelCount", f.channelCount);
    f.sampleFormat = sampleFormatOf(v, f.sampleFormat);
    if (f.sampleRate <= 0) f.sampleRate = def.sampleRate;
    if (f.channelCount <= 0) f.channelCount = def.channelCount;
    return f;
}

Json::Value fmtJson(const JZAudioFormat& f)
{
    static const char* kNames[] = { "uint8", "int16", "int32", "float" };
    Json::Value v;
    v["channelCount"] = f.channelCount;
    v["sampleRate"] = f.sampleRate;
    v["sampleFormat"] = (int)f.sampleFormat;
    const int idx = (int)f.sampleFormat;
    v["sampleFormatName"] = (idx >= 0 && idx <= 3) ? kNames[idx] : "unknown";
    v["bytesPerSample"] = f.bytesPerSample();
    v["frameBytes"] = f.bytesPerSample() * (f.channelCount > 0 ? f.channelCount : 1);
    return v;
}

// isInput 以"这份列表来自哪个方向"为准: 驱动的实现里输出列表的 isInput 曾误填 true(已修),
// 但静态库改动要重链才生效 —— 这里按列表语义给准值, 免得调用方被误导。
Json::Value deviceJson(const std::vector<JZAudioDeviceInfo>& list, bool isInput)
{
    Json::Value arr(Json::arrayValue);
    for (size_t i = 0; i < list.size(); ++i)
    {
        Json::Value d;
        d["id"] = list[i].id;
        d["name"] = list[i].name;
        d["isInput"] = isInput;
        arr.append(d);
    }
    return arr;
}
}  // namespace
#endif  // HAS_JZAudio

// ============================================================================
// 构造/析构/挂载（两版共用）
// ============================================================================
JZJSAudioModule::JZJSAudioModule() = default;

JZJSAudioModule::~JZJSAudioModule()
{
    releaseAll();
}

void JZJSAudioModule::initEngine(JZJSEngine* engine)
{
    m_engine = engine;
    if (!engine)
        return;

    JSValue global = engine->globalObject();

    // 复用已存在的 __jz(别覆盖 net/core/cv 建好的子命名空间)
    JSValue jz = engine->getProperty(global, "__jz");
    const bool fromGet = !JS_IsUndefined(jz);
    if (!fromGet)
    {
        jz = engine->createObject();
        engine->setProperty(global, "__jz", jz);
    }

    JSValue ns = engine->createObject();
    engine->registFunction(ns, "available", jsbind::createSelfFunction(this, &JZJSAudioModule::available));
    engine->registFunction(ns, "devices", jsbind::createSelfFunction(this, &JZJSAudioModule::devices));
    engine->registFunction(ns, "openInput", jsbind::createSelfFunction(this, &JZJSAudioModule::openInput));
    engine->registFunction(ns, "inputInfo", jsbind::createSelfFunction(this, &JZJSAudioModule::inputInfo));
    engine->registFunction(ns, "readInput", jsbind::createSelfFunction(this, &JZJSAudioModule::readInput));
    engine->registFunction(ns, "closeInput", jsbind::createSelfFunction(this, &JZJSAudioModule::closeInput));
    engine->registFunction(ns, "openOutput", jsbind::createSelfFunction(this, &JZJSAudioModule::openOutput));
    engine->registFunction(ns, "writeOutput", jsbind::createSelfFunction(this, &JZJSAudioModule::writeOutput));
    engine->registFunction(ns, "outputFinished", jsbind::createSelfFunction(this, &JZJSAudioModule::outputFinished));
    engine->registFunction(ns, "closeOutput", jsbind::createSelfFunction(this, &JZJSAudioModule::closeOutput));
    engine->registFunction(ns, "wavInfo", jsbind::createSelfFunction(this, &JZJSAudioModule::wavInfo));
    engine->registFunction(ns, "wavReadAll", jsbind::createSelfFunction(this, &JZJSAudioModule::wavReadAll));
    engine->registFunction(ns, "wavWrite", jsbind::createSelfFunction(this, &JZJSAudioModule::wavWrite));
    engine->registFunction(ns, "transcode", jsbind::createSelfFunction(this, &JZJSAudioModule::transcode));
    engine->registFunction(ns, "releaseAll", jsbind::createSelfFunction(this, &JZJSAudioModule::releaseAll));

    engine->setProperty(jz, "audio", ns);

    if (fromGet)
        JS_FreeValue(engine->context(), jz);   // 释放 getProperty 多 dup 的引用
}

void JZJSAudioModule::deinitEngine()
{
    releaseAll();
    m_engine = nullptr;
}

JZJSAudioModule::InputSlot* JZJSAudioModule::findInput(int id)
{
    std::map<int, InputSlot*>::iterator it = m_inputs.find(id);
    return (it == m_inputs.end()) ? nullptr : it->second;
}

JZJSAudioModule::OutputSlot* JZJSAudioModule::findOutput(int id)
{
    std::map<int, OutputSlot*>::iterator it = m_outputs.find(id);
    return (it == m_outputs.end()) ? nullptr : it->second;
}

Json::Value JZJSAudioModule::notBuilt() const
{
    Json::Value v;
    v["error"] = "本构建未编入 JZAudio(HAS_JZAudio 未定义)";
    return v;
}

#ifdef HAS_JZAudio
// ============================================================================
// 编入 JZAudio 的版本
// ============================================================================
bool JZJSAudioModule::available() { return true; }

Json::Value JZJSAudioModule::devices()
{
    Json::Value v;
    v["inputs"] = deviceJson(JZAudioDeviceInfo::inputList(), true);
    v["outputs"] = deviceJson(JZAudioDeviceInfo::outputList(), false);
    return v;
}

int JZJSAudioModule::openInput(const Json::Value& opts)
{
    const JZAudioFormat fmt = fmtOf(opts, JZAudioFormat());
    const std::string devId = strOf(opts, "deviceId", "");

    JZAudioInput* in = new JZAudioInput();
    in->setFormat(fmt);

    bool ok = false;
    if (!devId.empty())
    {
        const std::vector<JZAudioDeviceInfo> list = JZAudioDeviceInfo::inputList();
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].id == devId)
            {
                ok = in->open(list[i]);
                break;
            }
    }
    else
    {
        ok = in->open();                    // 默认设备(实现里取 inputList()[0])
    }
    if (!ok)
    {
        delete in;
        return 0;
    }

    InputSlot* slot = new InputSlot();
    slot->in = in;
    const int id = m_nextId++;
    m_inputs[id] = slot;

    // ⚠ 音频回调里**不碰 JS**: 只往带锁的缓冲里追加(见头文件说明)。
    //   实现在 Windows 侧经 runOnObject 投递到对象所属线程(= 引擎线程)执行, Linux 侧是定时器。
    connect(&in->sigReadyRead, [slot](const JZByteArray& data) {
        std::lock_guard<std::mutex> lk(slot->mu);
        if (slot->buf.size() + data.size() > kMaxBuffered)
        {
            const int drop = slot->buf.size() + data.size() - kMaxBuffered;
            slot->buf.remove(0, drop);
            slot->overrun = true;
        }
        slot->buf.append(data);
    });
    return id;
}

Json::Value JZJSAudioModule::inputInfo(int id)
{
    InputSlot* s = findInput(id);
    if (!s || !s->in)
        return notBuilt();
    Json::Value v = fmtJson(s->in->format());
    std::lock_guard<std::mutex> lk(s->mu);
    v["buffered"] = s->buf.size();
    v["overrun"] = s->overrun;
    return v;
}

JSValue JZJSAudioModule::readInput(int id)
{
    JSContext* ctx = m_engine ? m_engine->context() : nullptr;
    InputSlot* s = findInput(id);
    if (!ctx || !s)
        return ctx ? jsMakeUint8Array(ctx, JZByteArray()) : JS_UNDEFINED;

    JZByteArray out;
    {
        std::lock_guard<std::mutex> lk(s->mu);
        out.swap(s->buf);                   // 一次取空(swap, 不拷贝)
        s->overrun = false;
    }
    return jsMakeUint8Array(ctx, out);
}

bool JZJSAudioModule::closeInput(int id) { return dropInput(id); }

bool JZJSAudioModule::dropInput(int id)
{
    std::map<int, InputSlot*>::iterator it = m_inputs.find(id);
    if (it == m_inputs.end())
        return false;
    InputSlot* s = it->second;
    // 先销毁 input: 它一没, sigReadyRead 与回调 lambda 一起没了(回调不会再跑到这个 slot 上)
    delete s->in;
    s->in = nullptr;
    delete s;
    m_inputs.erase(it);
    return true;
}

int JZJSAudioModule::openOutput(const Json::Value& opts)
{
    const JZAudioFormat fmt = fmtOf(opts, JZAudioFormat());
    const std::string devId = strOf(opts, "deviceId", "");

    JZAudioOutput* out = new JZAudioOutput();
    out->setFormat(fmt);

    bool ok = false;
    if (!devId.empty())
    {
        const std::vector<JZAudioDeviceInfo> list = JZAudioDeviceInfo::outputList();
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].id == devId)
            {
                ok = out->open(list[i]);
                break;
            }
    }
    else
    {
        ok = out->open();
    }
    if (!ok)
    {
        delete out;
        return 0;
    }

    OutputSlot* slot = new OutputSlot();
    slot->out = out;
    const int id = m_nextId++;
    m_outputs[id] = slot;

    // 播完的标记: 只置标志, 由 JS 侧轮询 outputFinished()（同样不在信号里碰 JS）
    connect(&out->sigPlayFinish, [slot]() { slot->finished = true; });
    return id;
}

bool JZJSAudioModule::writeOutput(int id, JSValue data)
{
    OutputSlot* s = findOutput(id);
    if (!s || !s->out || !m_engine)
        return false;
    JZByteArray bytes;
    if (!jsValueToBytes(m_engine->context(), data, bytes))
        return false;
    if (bytes.isEmpty())
        return false;
    s->finished = false;
    return s->out->write(bytes);            // 队列满(4 块)会返回 false, 调用方自行重试
}

bool JZJSAudioModule::outputFinished(int id)
{
    OutputSlot* s = findOutput(id);
    if (!s)
        return false;
    const bool f = s->finished;
    s->finished = false;                    // 读一次就清
    return f;
}

bool JZJSAudioModule::closeOutput(int id)
{
    std::map<int, OutputSlot*>::iterator it = m_outputs.find(id);
    if (it == m_outputs.end())
        return false;
    OutputSlot* s = it->second;
    delete s->out;                          // 析构里会 close()
    s->out = nullptr;
    delete s;
    m_outputs.erase(it);
    return true;
}

Json::Value JZJSAudioModule::wavInfo(const std::string& path)
{
    JZWaveFile f;
    if (!f.open(path))
    {
        Json::Value v;
        v["error"] = "打不开 WAV: " + path;
        return v;
    }
    Json::Value v = fmtJson(f.format());
    v["dataSize"] = f.dataSize();
    const int frameBytes = f.format().bytesPerSample() * (f.format().channelCount > 0 ? f.format().channelCount : 1);
    v["frames"] = (frameBytes > 0) ? (f.dataSize() / frameBytes) : 0;
    f.close();
    return v;
}

JSValue JZJSAudioModule::wavReadAll(const std::string& path)
{
    JSContext* ctx = m_engine ? m_engine->context() : nullptr;
    if (!ctx)
        return JS_UNDEFINED;
    JZWaveFile f;
    if (!f.open(path))
        return jsMakeUint8Array(ctx, JZByteArray());
    JZByteArray data = f.readAll();
    f.close();
    return jsMakeUint8Array(ctx, data);
}

bool JZJSAudioModule::wavWrite(const std::string& path, const Json::Value& fmt, JSValue data)
{
    if (!m_engine)
        return false;
    JZByteArray bytes;
    if (!jsValueToBytes(m_engine->context(), data, bytes))
        return false;

    JZWaveFile f;
    if (!f.create(path, fmtOf(fmt, JZAudioFormat())))
        return false;
    const int written = f.write(bytes);
    f.close();
    return written >= 0;
}

JSValue JZJSAudioModule::transcode(JSValue data, const Json::Value& inFmt, const Json::Value& outFmt)
{
    JSContext* ctx = m_engine ? m_engine->context() : nullptr;
    if (!ctx)
        return JS_UNDEFINED;
    JZByteArray bytes;
    if (!jsValueToBytes(ctx, data, bytes))
        return jsMakeUint8Array(ctx, JZByteArray());
    const JZAudioFormat in = fmtOf(inFmt, JZAudioFormat());
    const JZAudioFormat out = fmtOf(outFmt, JZAudioFormat());
    // ⚠ audioTrans 内部按 int16 处理(只做线性重采样 + 单声道复制到多声道), 见 JZAudioUtils.cpp
    return jsMakeUint8Array(ctx, audioTrans(bytes, in, out));
}

int JZJSAudioModule::releaseAll()
{
    std::vector<int> inIds, outIds;
    for (std::map<int, InputSlot*>::iterator it = m_inputs.begin(); it != m_inputs.end(); ++it)
        inIds.push_back(it->first);
    for (std::map<int, OutputSlot*>::iterator it = m_outputs.begin(); it != m_outputs.end(); ++it)
        outIds.push_back(it->first);
    for (size_t i = 0; i < inIds.size(); ++i)
        dropInput(inIds[i]);
    for (size_t i = 0; i < outIds.size(); ++i)
        closeOutput(outIds[i]);
    return (int)(inIds.size() + outIds.size());
}

#else
// ============================================================================
// 未编入 JZAudio 的退化版: 形状一致、调用给明确失败(与 cv 的处理方式相同)
// ============================================================================
bool JZJSAudioModule::available() { return false; }
Json::Value JZJSAudioModule::devices() { return notBuilt(); }
int JZJSAudioModule::openInput(const Json::Value&) { return 0; }
Json::Value JZJSAudioModule::inputInfo(int) { return notBuilt(); }
JSValue JZJSAudioModule::readInput(int) { return JS_UNDEFINED; }
bool JZJSAudioModule::closeInput(int) { return false; }
int JZJSAudioModule::openOutput(const Json::Value&) { return 0; }
bool JZJSAudioModule::writeOutput(int, JSValue) { return false; }
bool JZJSAudioModule::outputFinished(int) { return false; }
bool JZJSAudioModule::closeOutput(int) { return false; }
Json::Value JZJSAudioModule::wavInfo(const std::string&) { return notBuilt(); }
JSValue JZJSAudioModule::wavReadAll(const std::string&) { return JS_UNDEFINED; }
bool JZJSAudioModule::wavWrite(const std::string&, const Json::Value&, JSValue) { return false; }
JSValue JZJSAudioModule::transcode(JSValue, const Json::Value&, const Json::Value&) { return JS_UNDEFINED; }
int JZJSAudioModule::releaseAll() { return 0; }
bool JZJSAudioModule::dropInput(int) { return false; }
bool JZJSAudioModule::dropOutput(int) { return false; }
#endif  // HAS_JZAudio
