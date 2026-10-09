// ————————————————————————————————————————————————————————————————————————————
// JZJSOnnxModule.cpp —— __jz.onnx.*（ONNX Runtime 推理 → JS）
//
// 一句话: 把"加载 .onnx、看它的输入输出、喂张量、拿张量"这三件事暴露给脚本, 让脚本能
//   跑任意模型(分类/检测/embedding), 不必为每个模型写一个 C++ 模块。
//
// 为什么数据走 JSON 数组: 第一版要的是"能跑通、形状清楚"; 大张量(几万元素以上)会明显变慢,
//   后面要改的是**二进制通道**(base64/内存句柄)与"按需取输出", 而不是这个接口的形状。
//
// 工具集: 本工程是 VS2015(v140 / C++14) ⇒ 只能用 ONNX Runtime 的 **C API**
//   (C++ 头 onnxruntime_cxx_api.h 要 C++17)。补丁的来龙去脉见
//   JZCv/paddleocr/onnx_engine.cpp 顶部 —— 那边是同一处 hack, 这里照抄。
// ————————————————————————————————————————————————————————————————————————————
// ⚠ NOMINMAX/WIN32_LEAN_AND_MEAN 必须早于第一处 <windows.h>(json/json.h 就会带进来),
//   否则 min/max 宏会漏进后面所有头(与 JZComputerUse.h / onnx_engine.cpp 同一个坑)。
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#endif

#include "JZJSOnnxModule.h"
#include "JZJSBind.h"                        // jsbind::createSelfFunction
#include <json/json.h>

#include <map>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cctype>                            // std::isalnum / tolower / isspace
#include <stdexcept>
#include <algorithm>

JZJSOnnxModule::JZJSOnnxModule() = default;
JZJSOnnxModule::~JZJSOnnxModule() = default;

#if JZ_ONNX
// ============================================================================
// 编入 ONNX Runtime 的版本
// ============================================================================
// 标准头必须先包含完, 再进 noexcept 补丁区(补丁期间任何标准头都可能被清掉 noexcept 而语法出错)
#include <functional>                        // MSVC 的 <numeric> 不保证带到 std::multiplies
#include <numeric>

// ⚠ 判据按**语言标准**而非工具集版本：本工程 CMAKE_CXX_STANDARD 14，用 VS2022 编也仍是
//   /std:c++14 ⇒ ORT 头把 NO_EXCEPTION(=noexcept) 用在函数指针 typedef 上仍报 C2279；
//   旧判据 `_MSC_VER < 1910`（只有 v140 命中）在 VS2022 下为假 ⇒ 补丁失效（2026-10-08 实测）。
//   完整来龙去脉见 JZCv/paddleocr/onnx_engine.cpp 顶部。
#if defined(_MSC_VER) && (!defined(_MSVC_LANG) ? (_MSC_VER < 1910) : (_MSVC_LANG < 201703L))
#  define noexcept
#  define JZ_ORT_V140_COMPAT 1
#endif
#include <onnxruntime_c_api.h>
#if defined(JZ_ORT_V140_COMPAT)
#  undef noexcept
#  undef JZ_ORT_V140_COMPAT
#endif

#ifdef _WIN32
#  include <windows.h>                        // MultiByteToWideChar (UTF-8 路径 -> ORTCHAR_T)
#endif

namespace {

// —— ORT 的 C API 一律返回 OrtStatus*(nullptr 才是成功): 统一在这里翻成异常 ——
void OrtCheck(const OrtApi* api, OrtStatus* status, const char* what) {
    if (!status) return;
    const char* msg = api->GetErrorMessage(status);
    const std::string text = msg ? msg : "unknown error";
    api->ReleaseStatus(status);
    throw std::runtime_error(std::string(what) + ": " + text);
}

// 进程级 Env(与 JZCv 那套一样建议单例)
OrtEnv* GlobalEnv(const OrtApi* api) {
    static OrtEnv* env = [](const OrtApi* a) -> OrtEnv* {
        OrtEnv* e = nullptr;
        OrtStatus* st = a->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "JZOnnx", &e);
        if (st) {
            const char* msg = a->GetErrorMessage(st);
            const std::string text = msg ? msg : "unknown error";
            a->ReleaseStatus(st);
            throw std::runtime_error("CreateEnv: " + text);
        }
        return e;
    }(api);
    return env;
}

// 模型路径在 Windows 下是 ORTCHAR_T(= wchar_t): 必须从 UTF-8 转过去
std::basic_string<ORTCHAR_T> ToOrtPath(const std::string& path) {
#ifdef _WIN32
    if (path.empty()) return std::basic_string<ORTCHAR_T>();
    const int wn = ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wn <= 1) return std::basic_string<ORTCHAR_T>();
    std::wstring wide((size_t)(wn - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wide[0], wn);
    return wide;
#else
    return path;
#endif
}

int64_t ElemCount(const std::vector<int64_t>& shape) {
    if (shape.empty()) return 0;
    return std::accumulate(shape.begin(), shape.end(), int64_t{ 1 }, std::multiplies<int64_t>());
}

// —— 张量类型名 <-> ORT 枚举 ——
std::string DtypeName(ONNXTensorElementDataType t) {
    switch (t) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:  return "float32";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE: return "float64";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:  return "int64";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:  return "int32";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16:  return "int16";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:   return "int8";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16: return "uint16";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:  return "uint8";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL:   return "bool";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING: return "string";
    default: return "unsupported";
    }
}
ONNXTensorElementDataType DtypeOf(const std::string& s, bool& ok) {
    ok = true;
    std::string k;
    for (char c : s) if (std::isalnum((unsigned char)c)) k.push_back((char)std::tolower((unsigned char)c));
    if (k.empty() || k == "float32" || k == "float") return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    if (k == "float64" || k == "double") return ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE;
    if (k == "int64" || k == "long")     return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
    if (k == "int32" || k == "int")      return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32;
    if (k == "int16")                    return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16;
    if (k == "int8")                     return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8;
    if (k == "uint16")                   return ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16;
    if (k == "uint8" || k == "byte")     return ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8;
    if (k == "bool")                     return ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL;
    ok = false;
    return ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
}
size_t DtypeSize(ONNXTensorElementDataType t) {
    switch (t) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:  return 4;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE: return 8;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:  return 8;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:  return 4;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16:  return 2;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16: return 2;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:   return 1;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:  return 1;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL:   return 1;
    default: return 0;
    }
}

// —— base64(自带; 与 JZJSCvModule 同一份实现) ——
const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
int b64Val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
bool b64Decode(const std::string& in, std::vector<uint8_t>& out) {
    std::string s = in;
    const size_t comma = s.find("base64,");
    if (comma != std::string::npos) s = s.substr(comma + 7);
    int buf = 0, bits = 0;
    for (char c : s) {
        if (std::isspace((unsigned char)c) || c == '"') continue;
        if (c == '=') break;
        const int v = b64Val(c);
        if (v < 0) return false;
        buf = (buf << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((uint8_t)((buf >> bits) & 0xFF)); }
    }
    return !out.empty();
}

// —— JSON 取参 ——
int intOf(const Json::Value& p, const char* k, int def) {
    return (p.isMember(k) && p[k].isNumeric()) ? p[k].asInt() : def;
}
std::string strOf(const Json::Value& p, const char* k, const char* def) {
    return (p.isMember(k) && p[k].isString()) ? p[k].asString() : std::string(def);
}

// 一个输入/输出的描述
struct TensorSpec {
    std::string name;
    ONNXTensorElementDataType dtype = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    std::vector<int64_t> shape;          // 动态维度是 -1
};

Json::Value SpecToJson(const TensorSpec& s) {
    Json::Value v;
    v["name"] = s.name;
    v["dtype"] = DtypeName(s.dtype);
    Json::Value sh(Json::arrayValue);
    for (int64_t d : s.shape) sh.append((Json::Int64)d);
    v["shape"] = sh;
    return v;
}

// 把 feed 的参数整理成"连续字节缓冲 + 形状 + 类型"; 失败抛异常(带清楚的说明)
void BuildInput(const std::string& name, const Json::Value& v, std::vector<uint8_t>& buf,
                std::vector<int64_t>& shape, ONNXTensorElementDataType& dtype)
{
    if (!v.isObject()) throw std::runtime_error("输入 '" + name + "' 必须是 {shape:[...], data:[...]}");
    if (!v.isMember("shape") || !v["shape"].isArray() || v["shape"].size() == 0)
        throw std::runtime_error("输入 '" + name + "' 缺少 shape（动态维度要在这里给具体值）");
    for (const auto& d : v["shape"]) {
        const int64_t n = d.asInt64();
        if (n <= 0) throw std::runtime_error("输入 '" + name + "' 的 shape 里有非正数（动态维度必须给具体值）");
        shape.push_back(n);
    }
    bool ok = true;
    dtype = v.isMember("dtype") ? DtypeOf(strOf(v, "dtype", "float32"), ok)
                                : ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    if (!ok) throw std::runtime_error("输入 '" + name + "' 的 dtype 不认识（支持 float32/float64/int64/int32/int16/int8/uint16/uint8/bool）");
    const int64_t want = ElemCount(shape);
    const size_t esz = DtypeSize(dtype);
    if (esz == 0) throw std::runtime_error("输入 '" + name + "' 的 dtype 是本版不支持的");

    buf.clear();
    if (v.isMember("dataB64")) {                       // 原始字节(base64): 大张量走这条
        std::vector<uint8_t> raw;
        if (!b64Decode(strOf(v, "dataB64", ""), raw))
            throw std::runtime_error("输入 '" + name + "' 的 dataB64 解不开");
        const size_t need = (size_t)want * esz;
        if (raw.size() < need)
            throw std::runtime_error("输入 '" + name + "' 的 dataB64 字节数不够: 有 " +
                                     std::to_string(raw.size()) + ", 需要 " + std::to_string(need));
        buf.assign(raw.begin(), raw.begin() + need);
        return;
    }
    if (!v.isMember("data") || !v["data"].isArray())
        throw std::runtime_error("输入 '" + name + "' 缺少 data（数组）或 dataB64");
    const Json::Value& d = v["data"];
    if ((int64_t)d.size() != want)
        throw std::runtime_error("输入 '" + name + "' 的数据个数与 shape 不符: " + std::to_string(d.size()) +
                                 " vs " + std::to_string(want));
    buf.resize((size_t)want * esz);
    for (int64_t i = 0; i < want; ++i) {
        const Json::Value& e = d[(Json::Value::ArrayIndex)i];
        // ⚠ 不能直接写 (float*)buf.data()[i]: 缓冲没保证 4/8 字节对齐 ⇒ memcpy 逐元素
        switch (dtype) {
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:  { const float    x = (float)e.asDouble();  memcpy(&buf[(size_t)i * 4], &x, 4); break; }
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE: { const double   x = e.asDouble();        memcpy(&buf[(size_t)i * 8], &x, 8); break; }
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:  { const int64_t  x = e.asInt64();         memcpy(&buf[(size_t)i * 8], &x, 8); break; }
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:  { const int32_t  x = (int32_t)e.asInt();  memcpy(&buf[(size_t)i * 4], &x, 4); break; }
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16:  { const int16_t  x = (int16_t)e.asInt();  memcpy(&buf[(size_t)i * 2], &x, 2); break; }
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16: { const uint16_t x = (uint16_t)e.asInt(); memcpy(&buf[(size_t)i * 2], &x, 2); break; }
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:   { const int8_t   x = (int8_t)e.asInt();   buf[(size_t)i] = (uint8_t)x; break; }
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:  { buf[(size_t)i] = (uint8_t)e.asInt();    break; }
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL:   { buf[(size_t)i] = e.asBool() ? 1 : 0;    break; }
        default: break;
        }
    }
}

// 把输出的张量转成 JSON 数组（本版只做"整块搬回去"; 大输出该按需取, 见文件头）
Json::Value OutputToJson(const OrtApi* api, OrtValue* ov, const std::string& name) {
    Json::Value v;
    v["name"] = name;
    OrtTensorTypeAndShapeInfo* info = nullptr;
    OrtCheck(api, api->GetTensorTypeAndShape(ov, &info), "GetTensorTypeAndShape");
    ONNXTensorElementDataType dtype = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    api->GetTensorElementType(info, &dtype);
    size_t dims = 0;
    api->GetDimensionsCount(info, &dims);
    std::vector<int64_t> shape(dims);
    if (dims > 0) api->GetDimensions(info, shape.data(), dims);
    size_t elem = 0;
    api->GetTensorShapeElementCount(info, &elem);
    api->ReleaseTensorTypeAndShapeInfo(info);

    v["dtype"] = DtypeName(dtype);
    Json::Value sh(Json::arrayValue);
    for (int64_t d : shape) sh.append((Json::Int64)d);
    v["shape"] = sh;

    void* raw = nullptr;
    OrtCheck(api, api->GetTensorMutableData(ov, &raw), "GetTensorMutableData");
    if (!raw || elem == 0) { v["data"] = Json::Value(Json::arrayValue); return v; }

    Json::Value arr(Json::arrayValue);
    switch (dtype) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:  { const float* p = (const float*)raw;  for (size_t i = 0; i < elem; ++i) arr.append((double)p[i]); break; }
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE: { const double* p = (const double*)raw; for (size_t i = 0; i < elem; ++i) arr.append(p[i]); break; }
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:  { const int64_t* p = (const int64_t*)raw; for (size_t i = 0; i < elem; ++i) arr.append((Json::Int64)p[i]); break; }
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:  { const int32_t* p = (const int32_t*)raw; for (size_t i = 0; i < elem; ++i) arr.append(p[i]); break; }
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16:  { const int16_t* p = (const int16_t*)raw; for (size_t i = 0; i < elem; ++i) arr.append((int)p[i]); break; }
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16: { const uint16_t* p = (const uint16_t*)raw; for (size_t i = 0; i < elem; ++i) arr.append((int)p[i]); break; }
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:   { const int8_t* p = (const int8_t*)raw; for (size_t i = 0; i < elem; ++i) arr.append((int)p[i]); break; }
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:  { const uint8_t* p = (const uint8_t*)raw; for (size_t i = 0; i < elem; ++i) arr.append((int)p[i]); break; }
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL:   { const uint8_t* p = (const uint8_t*)raw; for (size_t i = 0; i < elem; ++i) arr.append(p[i] != 0); break; }
    default:
        // string / 复杂类型: 本版不转(明说, 不糊一个空数组让人以为成功)
        v["note"] = "这个输出的类型本版不支持搬回 JS（" + DtypeName(dtype) + "）";
        return v;
    }
    v["data"] = arr;
    return v;
}

Json::Value jFail(const std::string& msg) { Json::Value v; v["ok"] = false; v["error"] = msg; return v; }
Json::Value jOk() { Json::Value v; v["ok"] = true; return v; }

} // namespace

// Impl: 会话表
struct JZJSOnnxModule::Impl
{
    struct Entry {
        OrtSession* session = nullptr;
        std::vector<TensorSpec> inputs;
        std::vector<TensorSpec> outputs;
    };
    const OrtApi* api = nullptr;              // 进程级, 与 Env 一起活
    std::map<int, Entry> sessions;
    int nextId = 1;

    ~Impl() {
        if (api) {
            for (auto& kv : sessions) if (kv.second.session) api->ReleaseSession(kv.second.session);
        }
        sessions.clear();
    }
};

bool JZJSOnnxModule::jsAvailable() { return true; }

std::string JZJSOnnxModule::jsVersion() {
    const OrtApiBase* base = OrtGetApiBase();
    const char* v = base ? base->GetVersionString() : nullptr;
    return v ? v : std::string();
}

Json::Value JZJSOnnxModule::jsLoad(const std::string& path, int numThreads) {
    if (!m_impl) return jFail("模块未初始化");
    if (path.empty()) return jFail("没给模型路径");

    const OrtApi* api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!api) return jFail("拿不到 OrtApi（ORT 版本与头不匹配）");

    OrtSessionOptions* opts = nullptr;
    OrtSession* session = nullptr;
    try {
        OrtCheck(api, api->CreateSessionOptions(&opts), "CreateSessionOptions");
        OrtCheck(api, api->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL),
                 "SetSessionGraphOptimizationLevel");
        if (numThreads > 0) {
            OrtCheck(api, api->SetIntraOpNumThreads(opts, numThreads), "SetIntraOpNumThreads");
            OrtCheck(api, api->SetInterOpNumThreads(opts, 1), "SetInterOpNumThreads");
        }
        // 只有 CPU EP（要 CUDA/DML 得另接 provider, 本版不做）
        const std::basic_string<ORTCHAR_T> osPath = ToOrtPath(path);
        OrtCheck(api, api->CreateSession(GlobalEnv(api), osPath.c_str(), opts, &session), "CreateSession");
        api->ReleaseSessionOptions(opts);
        opts = nullptr;

        Impl::Entry e;
        e.session = session;
        // 输入/输出的名字+类型+形状都由 ORT 给(动态维度是 -1)
        for (int side = 0; side < 2; ++side) {
            std::vector<TensorSpec>* dst = side == 0 ? &e.inputs : &e.outputs;
            size_t count = 0;
            OrtCheck(api, side == 0 ? api->SessionGetInputCount(session, &count)
                                    : api->SessionGetOutputCount(session, &count), "SessionGetCount");
            for (size_t i = 0; i < count; ++i) {
                OrtAllocator* alloc = nullptr;
                OrtCheck(api, api->GetAllocatorWithDefaultOptions(&alloc), "GetAllocatorWithDefaultOptions");
                char* name = nullptr;
                OrtCheck(api, side == 0 ? api->SessionGetInputName(session, i, alloc, &name)
                                        : api->SessionGetOutputName(session, i, alloc, &name), "SessionGetName");
                TensorSpec spec;
                spec.name = name ? name : "";
                if (name) api->AllocatorFree(alloc, name);

                OrtTypeInfo* ti = nullptr;
                OrtCheck(api, side == 0 ? api->SessionGetInputTypeInfo(session, i, &ti)
                                        : api->SessionGetOutputTypeInfo(session, i, &ti), "SessionGetTypeInfo");
                const OrtTensorTypeAndShapeInfo* tsi = nullptr;
                if (api->CastTypeInfoToTensorInfo(ti, &tsi) == nullptr && tsi) {
                    api->GetTensorElementType(tsi, &spec.dtype);
                    size_t dims = 0;
                    if (api->GetDimensionsCount(tsi, &dims) == nullptr && dims > 0) {
                        spec.shape.resize(dims);
                        api->GetDimensions(tsi, spec.shape.data(), dims);
                    }
                }
                api->ReleaseTypeInfo(ti);
                dst->push_back(spec);
            }
        }

        if (!m_impl->api) m_impl->api = api;
        const int id = m_impl->nextId++;
        m_impl->sessions[id] = std::move(e);

        Json::Value v = jOk();
        v["session"] = id;
        v["model"] = path;
        Json::Value ins(Json::arrayValue), outs(Json::arrayValue);
        for (const auto& s : m_impl->sessions[id].inputs) ins.append(SpecToJson(s));
        for (const auto& s : m_impl->sessions[id].outputs) outs.append(SpecToJson(s));
        v["inputs"] = ins;
        v["outputs"] = outs;
        return v;
    } catch (const std::exception& ex) {
        if (opts) api->ReleaseSessionOptions(opts);
        if (session) api->ReleaseSession(session);
        return jFail(std::string("加载模型失败: ") + ex.what());
    }
}

Json::Value JZJSOnnxModule::jsInfo(int id) {
    if (!m_impl) return jFail("模块未初始化");
    auto it = m_impl->sessions.find(id);
    if (it == m_impl->sessions.end()) return jFail("无效的会话句柄 " + std::to_string(id));
    Json::Value v = jOk();
    v["session"] = id;
    Json::Value ins(Json::arrayValue), outs(Json::arrayValue);
    for (const auto& s : it->second.inputs) ins.append(SpecToJson(s));
    for (const auto& s : it->second.outputs) outs.append(SpecToJson(s));
    v["inputs"] = ins;
    v["outputs"] = outs;
    return v;
}

Json::Value JZJSOnnxModule::jsRun(int id, const std::string& feedsJson) {
    if (!m_impl) return jFail("模块未初始化");
    auto it = m_impl->sessions.find(id);
    if (it == m_impl->sessions.end()) return jFail("无效的会话句柄 " + std::to_string(id));
    const Impl::Entry& e = it->second;
    const OrtApi* api = m_impl->api;
    if (!api || !e.session) return jFail("这个句柄没有可用的会话");

    Json::Value feeds;
    if (!feedsJson.empty()) {
        Json::Reader reader;
        if (!reader.parse(feedsJson, feeds)) return jFail("feeds 不是合法 JSON");
    }
    if (!feeds.isObject()) feeds = Json::Value(Json::objectValue);

    // 缓冲要在 Run 期间一直有效(ORT 不拷贝数据) —— 先按输入个数定好, 之后只改内容不扩容
    std::vector<std::vector<uint8_t>> bufs(e.inputs.size());
    std::vector<OrtValue*> inputs(e.inputs.size(), nullptr);
    std::vector<OrtValue*> outputs(e.outputs.size(), nullptr);
    std::vector<std::string> inNames, outNames;
    OrtMemoryInfo* memInfo = nullptr;
    Json::Value result = jOk();

    try {
        OrtCheck(api, api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memInfo),
                 "CreateCpuMemoryInfo");

        for (size_t i = 0; i < e.inputs.size(); ++i) {
            const TensorSpec& spec = e.inputs[i];
            if (!feeds.isMember(spec.name))
                throw std::runtime_error("缺输入 '" + spec.name + "'（feeds 里要用模型给的输入名做键）");
            std::vector<int64_t> shape;
            ONNXTensorElementDataType dtype = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
            BuildInput(spec.name, feeds[spec.name], bufs[i], shape, dtype);
            // 模型声明的类型是准的: 类型不一致时以"用户传的"为准会静默出错, 所以这里对齐检查
            if (spec.dtype != ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED &&
                spec.dtype != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT && spec.dtype != dtype)
                throw std::runtime_error("输入 '" + spec.name + "' 的类型不符: 模型要 " +
                                         DtypeName(spec.dtype) + ", 你给了 " + DtypeName(dtype));
            OrtCheck(api, api->CreateTensorWithDataAsOrtValue(
                             memInfo, bufs[i].data(), bufs[i].size(), shape.data(), shape.size(), dtype, &inputs[i]),
                     "CreateTensorWithDataAsOrtValue");
            inNames.push_back(spec.name);
        }

        std::vector<const char*> inC, outC;
        for (const auto& s : inNames) inC.push_back(s.c_str());
        for (const auto& s : e.outputs) outC.push_back(s.name.c_str());

        // ⚠ C API 里这个函数叫 Run（C++ 包装层才叫 Session::Run）
        OrtCheck(api, api->Run(e.session, nullptr, inC.data(),
                               const_cast<const OrtValue* const*>(inputs.data()), inputs.size(),
                               outC.data(), outC.size(), outputs.data()), "Run");

        Json::Value arr(Json::arrayValue);
        for (size_t i = 0; i < e.outputs.size(); ++i)
            arr.append(OutputToJson(api, outputs[i], e.outputs[i].name));
        result["outputs"] = arr;
    } catch (const std::exception& ex) {
        result = jFail(std::string("推理失败: ") + ex.what());
    }

    for (OrtValue* ov : inputs)  if (ov) api->ReleaseValue(ov);
    for (OrtValue* ov : outputs) if (ov) api->ReleaseValue(ov);
    if (memInfo) api->ReleaseMemoryInfo(memInfo);
    return result;
}

bool JZJSOnnxModule::jsRelease(int id) {
    if (!m_impl) return false;
    auto it = m_impl->sessions.find(id);
    if (it == m_impl->sessions.end()) return false;
    if (m_impl->api && it->second.session) m_impl->api->ReleaseSession(it->second.session);
    m_impl->sessions.erase(it);
    return true;
}

int JZJSOnnxModule::jsReleaseAll() {
    if (!m_impl) return 0;
    const int n = (int)m_impl->sessions.size();
    if (m_impl->api)
        for (auto& kv : m_impl->sessions) if (kv.second.session) m_impl->api->ReleaseSession(kv.second.session);
    m_impl->sessions.clear();
    return n;
}

int JZJSOnnxModule::jsCount() { return m_impl ? (int)m_impl->sessions.size() : 0; }

#else
// ============================================================================
// 没编 ONNX Runtime 的版本: 一律报"未编入", 绝不假装成功
// ============================================================================
struct JZJSOnnxModule::Impl {};

namespace {
const char* kNoOrt = "本构建未编入 ONNX Runtime(JZ_ONNX): __jz.onnx 不可用";
Json::Value jFail(const std::string& msg) { Json::Value v; v["ok"] = false; v["error"] = msg; return v; }
} // namespace

bool JZJSOnnxModule::jsAvailable() { return false; }
std::string JZJSOnnxModule::jsVersion() { return std::string(); }
Json::Value JZJSOnnxModule::jsLoad(const std::string&, int) { return jFail(kNoOrt); }
Json::Value JZJSOnnxModule::jsInfo(int) { return jFail(kNoOrt); }
Json::Value JZJSOnnxModule::jsRun(int, const std::string&) { return jFail(kNoOrt); }
bool JZJSOnnxModule::jsRelease(int) { return false; }
int JZJSOnnxModule::jsReleaseAll() { return 0; }
int JZJSOnnxModule::jsCount() { return 0; }

#endif

// ============================================================================
// 注册(两版共用): __jz.onnx.*
// ============================================================================
void JZJSOnnxModule::initEngine(JZJSEngine* engine) {
    m_engine = engine;
    if (!m_impl) m_impl.reset(new Impl());
    JSValue global = m_engine->globalObject();

    // 复用已存在的 __jz(不要覆盖 net/core 建好的子命名空间; 与 JZJSComputerUseModule 同款)
    JSValue jz = m_engine->getProperty(global, "__jz");
    const bool fromGet = !JS_IsUndefined(jz);
    if (!fromGet) {
        jz = m_engine->createObject();
        m_engine->setProperty(global, "__jz", jz);
    }

    JSValue ns = m_engine->createObject();
    m_engine->registFunction(ns, "available",  jsbind::createSelfFunction(this, &JZJSOnnxModule::jsAvailable));
    m_engine->registFunction(ns, "version",    jsbind::createSelfFunction(this, &JZJSOnnxModule::jsVersion));
    m_engine->registFunction(ns, "load",       jsbind::createSelfFunction(this, &JZJSOnnxModule::jsLoad));
    m_engine->registFunction(ns, "info",       jsbind::createSelfFunction(this, &JZJSOnnxModule::jsInfo));
    m_engine->registFunction(ns, "run",        jsbind::createSelfFunction(this, &JZJSOnnxModule::jsRun));
    m_engine->registFunction(ns, "release",    jsbind::createSelfFunction(this, &JZJSOnnxModule::jsRelease));
    m_engine->registFunction(ns, "releaseAll", jsbind::createSelfFunction(this, &JZJSOnnxModule::jsReleaseAll));
    m_engine->registFunction(ns, "count",      jsbind::createSelfFunction(this, &JZJSOnnxModule::jsCount));
    m_engine->setProperty(jz, "onnx", ns);

    // 释放 getProperty 多 dup 的引用, 避免 engine 析构时 gc_obj_list 非空断言
    if (fromGet) JS_FreeValue(m_engine->context(), jz);
}

void JZJSOnnxModule::deinitEngine() {
    // 直接析构 Impl: 连同会话表一起没了 ⇒ 所有 OrtSession 都被 Release
    // (⚠ 不要写 m_impl->sessions.clear(): 退化版没有这个成员)
    m_impl.reset();
    m_engine = nullptr;
}
