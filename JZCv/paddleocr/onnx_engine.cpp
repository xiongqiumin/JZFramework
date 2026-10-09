// ⚠ NOMINMAX/WIN32_LEAN_AND_MEAN 必须早于第一处 <windows.h>(OpenCV 的头会带进来),
//   否则 min/max 宏会漏进后面所有头 (与 JZComputerUse.h 同一个坑)。
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#endif

#include "onnx_engine.h"

// ---------------------------------------------------------------------------
// 工具集兼容: 让 MSVC v140(VS2015) 能编 ONNX Runtime 的 C API 头
//
// onnxruntime_c_api.h 把 NO_EXCEPTION 定义成了 `noexcept`(条件是 _MSC_VER >= 1900,
// v140 正好命中), 而它被用在**函数指针 typedef** 里(ORT_API_T / OrtThreadPoolWorkXxxFn)。
// "noexcept 属于函数类型的一部分" 是 C++17 才有的规则 => v140 直接报 C2279。
// 这里把 noexcept 临时抹成空宏(MSVC 的预处理器允许给关键字定义宏), typedef 就退回成
// 普通函数指针。两点约束:
//   1. 必须在 ORT 头**之前**定义, 之后立刻 #undef —— 否则整个翻译单元的 noexcept 都没了;
//   2. 标准头里大量出现 `noexcept(expr)` 形式, 被清掉会变成语法错误, 所以那些头必须先
//      包含完(上面的 onnx_engine.h 已带 <string>/<vector>/<memory>/<opencv2/core.hpp>),
//      靠 include guard 让 ORT 头(只包含 C 头)在 hack 期间包含不了它们。
// ⚠ 判据必须按**语言标准**，不能按工具集版本(_MSC_VER)：
//   本工程是 CMAKE_CXX_STANDARD 14 —— 用 VS2022 编也还是 /std:c++14，ORT 头的
//   NO_EXCEPTION(=noexcept) 照样落在函数指针 typedef 上，照样 C2279。旧判据
//   `_MSC_VER < 1910`（只有 v140 命中）在 VS2022 下为假 ⇒ 补丁不生效（2026-10-08 实测）。
//   _MSVC_LANG 是 MSVC 自报的**实际标准**(c++14 = 201402L)；VS2015 早期版本没有该宏，回落按 _MSC_VER 判。
// ---------------------------------------------------------------------------
#if defined(_MSC_VER) && (!defined(_MSVC_LANG) ? (_MSC_VER < 1910) : (_MSVC_LANG < 201703L))
#  define noexcept
#  define JZ_ORT_V140_COMPAT 1
#endif
#include <onnxruntime_c_api.h>
#if defined(JZ_ORT_V140_COMPAT)
#  undef noexcept
#  undef JZ_ORT_V140_COMPAT
#endif

#include <functional>   // std::multiplies (MSVC 的 <numeric> 不保证带到)
#include <iostream>
#include <numeric>
#include <stdexcept>
#ifdef _WIN32
#  include <windows.h>    // MultiByteToWideChar (UTF-8 路径 -> ORTCHAR_T)
#endif

namespace PaddleOCR
{
    namespace
    {
        // ORT 的 C API 每次都返回 OrtStatus*(nullptr 才是成功) —— 统一在这里翻成异常
        void OrtCheck(const OrtApi *api, OrtStatus *status, const char *what)
        {
            if (!status)
                return;
            const char *msg = api->GetErrorMessage(status);
            const std::string text = msg ? msg : "unknown error";
            api->ReleaseStatus(status);
            throw std::runtime_error(std::string(what) + ": " + text);
        }

        // Ort::Env 的 C API 版: 建议进程内单例(原实现也是 static Ort::Env)
        OrtEnv *GlobalEnv(const OrtApi *api)
        {
            static OrtEnv *env = [](const OrtApi *a) -> OrtEnv * {
                OrtEnv *e = nullptr;
                OrtStatus *status = a->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "PaddleOCR", &e);
                if (status)
                {
                    const char *msg = a->GetErrorMessage(status);
                    const std::string text = msg ? msg : "unknown error";
                    a->ReleaseStatus(status);
                    throw std::runtime_error("CreateEnv: " + text);
                }
                return e;
            }(api);
            return env;
        }

        int64_t ElementCount(const std::vector<int64_t> &shape)
        {
            if (shape.empty())
                return 0;
            return std::accumulate(shape.begin(), shape.end(), int64_t{1},
                                   std::multiplies<int64_t>());
        }

        // Ort::Session 的模型路径类型是 ORTCHAR_T: **Windows 下是 wchar_t**(Linux 是 char)。
        // 这份内核原来在 Linux 上跑(路径直接传 char*), 拿到 Windows 编译就会报
        // "没有重载函数可以转换所有参数类型"(C2665)。这里统一转成 ORTCHAR_T, 两个平台都对。
        std::basic_string<ORTCHAR_T> ToOrtPath(const std::string &model_path)
        {
#ifdef _WIN32
            if (model_path.empty())
                return std::basic_string<ORTCHAR_T>();
            const int wn = ::MultiByteToWideChar(CP_UTF8, 0, model_path.c_str(), -1, nullptr, 0);
            if (wn <= 1)
                return std::basic_string<ORTCHAR_T>();
            std::wstring wide((size_t)(wn - 1), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, model_path.c_str(), -1, &wide[0], wn);
            return wide;
#else
            return model_path;
#endif
        }
    } // namespace

    struct OnnxEngine::Impl
    {
        const OrtApi *api = nullptr;
        OrtSession *session = nullptr;          // 进程级 Env 归 GlobalEnv 管, 这里不管
        std::vector<std::string> input_names;
        std::vector<std::string> output_names;

        ~Impl()
        {
            if (api && session)
                api->ReleaseSession(session);
        }
    };

    OnnxEngine::OnnxEngine() : impl_(new Impl()) {}

    OnnxEngine::~OnnxEngine() = default;

    bool OnnxEngine::IsLoaded() const
    {
        return impl_ && impl_->session != nullptr;
    }

    const std::vector<std::string> &OnnxEngine::InputNames() const
    {
        return impl_->input_names;
    }

    const std::vector<std::string> &OnnxEngine::OutputNames() const
    {
        return impl_->output_names;
    }

    bool OnnxEngine::LoadModel(const std::string &model_path, int num_threads)
    {
        const OrtApi *api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
        if (!api)
        {
            std::cerr << "[OnnxEngine] 拿不到 OrtApi(版本不匹配): " << model_path << std::endl;
            return false;
        }
        impl_->api = api;

        OrtSessionOptions *opts = nullptr;
        OrtSession *session = nullptr;
        try
        {
            OrtCheck(api, api->CreateSessionOptions(&opts), "CreateSessionOptions");
            OrtCheck(api, api->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL),
                     "SetSessionGraphOptimizationLevel");
            if (num_threads > 0)
            {
                OrtCheck(api, api->SetIntraOpNumThreads(opts, num_threads), "SetIntraOpNumThreads");
                OrtCheck(api, api->SetInterOpNumThreads(opts, 1), "SetInterOpNumThreads");
            }

            const std::basic_string<ORTCHAR_T> os_path = ToOrtPath(model_path);
            OrtCheck(api, api->CreateSession(GlobalEnv(api), os_path.c_str(), opts, &session),
                     "CreateSession");

            OrtAllocator *allocator = nullptr;
            OrtCheck(api, api->GetAllocatorWithDefaultOptions(&allocator),
                     "GetAllocatorWithDefaultOptions");

            // 名字由 ORT 用 allocator 分配, 拷进 std::string 后必须还回去
            impl_->input_names.clear();
            impl_->output_names.clear();

            size_t count = 0;
            OrtCheck(api, api->SessionGetInputCount(session, &count), "SessionGetInputCount");
            for (size_t i = 0; i < count; ++i)
            {
                char *name = nullptr;
                OrtCheck(api, api->SessionGetInputName(session, i, allocator, &name),
                         "SessionGetInputName");
                impl_->input_names.emplace_back(name ? name : "");
                if (name)
                    api->AllocatorFree(allocator, name);
            }

            count = 0;
            OrtCheck(api, api->SessionGetOutputCount(session, &count), "SessionGetOutputCount");
            for (size_t i = 0; i < count; ++i)
            {
                char *name = nullptr;
                OrtCheck(api, api->SessionGetOutputName(session, i, allocator, &name),
                         "SessionGetOutputName");
                impl_->output_names.emplace_back(name ? name : "");
                if (name)
                    api->AllocatorFree(allocator, name);
            }
        }
        catch (const std::exception &e)
        {
            std::cerr << "[OnnxEngine] 加载失败 " << model_path << ": " << e.what() << std::endl;
            if (session)
                api->ReleaseSession(session);
            if (opts)
                api->ReleaseSessionOptions(opts);
            return false;
        }

        if (opts)
            api->ReleaseSessionOptions(opts);
        impl_->session = session;

        std::cout << "[OnnxEngine] " << model_path << "  in=" << impl_->input_names[0]
                  << "  out=" << impl_->output_names[0] << std::endl;
        return true;
    }

    std::vector<float> OnnxEngine::Run(const float *data, const std::vector<int64_t> &shape,
                                       std::vector<int64_t> &out_shape, size_t out_idx)
    {
        out_shape.clear();
        if (!impl_->session)
            return {};

        const int64_t n = ElementCount(shape);
        if (n <= 0)
            return {};

        const OrtApi *api = impl_->api;
        OrtMemoryInfo *mem_info = nullptr;
        OrtValue *input = nullptr;
        OrtTensorTypeAndShapeInfo *info = nullptr;
        std::vector<OrtValue *> outputs(impl_->output_names.size(), nullptr);
        std::vector<float> result;

        try
        {
            OrtCheck(api,
                     api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_info),
                     "CreateCpuMemoryInfo");
            OrtCheck(api,
                     api->CreateTensorWithDataAsOrtValue(
                         mem_info, const_cast<float *>(data), static_cast<size_t>(n) * sizeof(float),
                         shape.data(), shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input),
                     "CreateTensorWithDataAsOrtValue");

            std::vector<const char *> in_names, out_names;
            in_names.reserve(impl_->input_names.size());
            out_names.reserve(impl_->output_names.size());
            for (auto &s : impl_->input_names)
                in_names.push_back(s.c_str());
            for (auto &s : impl_->output_names)
                out_names.push_back(s.c_str());

            // ⚠ C API 里这个函数叫 Run(C++ 包装层才叫 Session::Run)
            OrtCheck(api,
                     api->Run(impl_->session, nullptr, in_names.data(), &input, 1,
                              out_names.data(), out_names.size(), outputs.data()),
                     "Run");

            if (out_idx >= outputs.size())
                throw std::runtime_error("输出下标越界: " + std::to_string(out_idx) + " / " +
                                         std::to_string(outputs.size()));

            OrtCheck(api, api->GetTensorTypeAndShape(outputs[out_idx], &info),
                     "GetTensorTypeAndShape");
            size_t dim_count = 0;
            OrtCheck(api, api->GetDimensionsCount(info, &dim_count), "GetDimensionsCount");
            out_shape.resize(dim_count);
            if (dim_count > 0)
                OrtCheck(api, api->GetDimensions(info, out_shape.data(), dim_count), "GetDimensions");
            size_t elem = 0;
            OrtCheck(api, api->GetTensorShapeElementCount(info, &elem), "GetTensorShapeElementCount");

            void *raw = nullptr;
            OrtCheck(api, api->GetTensorMutableData(outputs[out_idx], &raw), "GetTensorMutableData");
            const float *p = static_cast<const float *>(raw);
            if (p && elem > 0)
                result.assign(p, p + elem);
        }
        catch (const std::exception &e)
        {
            std::cerr << "[OnnxEngine] 推理失败: " << e.what() << std::endl;
            result.clear();
            out_shape.clear();
        }

        for (OrtValue *v : outputs)
        {
            if (v)
                api->ReleaseValue(v);
        }
        if (info)
            api->ReleaseTensorTypeAndShapeInfo(info);
        if (input)
            api->ReleaseValue(input);
        if (mem_info)
            api->ReleaseMemoryInfo(mem_info);

        return result;
    }

    std::vector<float> OnnxEngine::Run(const cv::Mat &blob, std::vector<int64_t> &out_shape,
                                       size_t out_idx)
    {
        std::vector<int64_t> shape;
        shape.reserve(blob.dims);
        for (int i = 0; i < blob.dims; ++i)
            shape.push_back(blob.size[i]);
        return Run(reinterpret_cast<const float *>(blob.data), shape, out_shape, out_idx);
    }

} // namespace PaddleOCR
