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

#include <iostream>
#include <numeric>
#ifdef _WIN32
#  include <windows.h>    // MultiByteToWideChar (UTF-8 路径 -> ORTCHAR_T)
#endif

namespace PaddleOCR
{

    namespace
    {
        // Ort::Env 建议进程内单例
        Ort::Env &GlobalEnv()
        {
            static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "PaddleOCR");
            return env;
        }

        int64_t ElementCount(const std::vector<int64_t> &shape)
        {
            if (shape.empty())
                return 0;
            return std::accumulate(shape.begin(), shape.end(), int64_t{1},
                                   std::multiplies<int64_t>());
        }
    } // namespace

    bool OnnxEngine::LoadModel(const std::string &model_path, int num_threads)
    {
        try
        {
            Ort::SessionOptions opts;
            opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            if (num_threads > 0)
            {
                opts.SetIntraOpNumThreads(num_threads);
                opts.SetInterOpNumThreads(1);
            }

            // ⚠ Ort::Session 的模型路径类型是 ORTCHAR_T: **Windows 下是 wchar_t**(Linux 是 char)。
            //   这份内核原来在 Linux 上跑(路径直接传 char*), 拿到 Windows 编译就是
            //   "Ort::Session::Session: 没有重载函数可以转换所有参数类型"(C2665)。
            //   这里统一转成 ORTCHAR_T, 两个平台都对。
            std::basic_string<ORTCHAR_T> os_path;
#ifdef _WIN32
            {
                const int wn = ::MultiByteToWideChar(CP_UTF8, 0, model_path.c_str(), -1, nullptr, 0);
                if (wn > 1) {
                    std::wstring wide((size_t)(wn - 1), L'\0');
                    ::MultiByteToWideChar(CP_UTF8, 0, model_path.c_str(), -1, &wide[0], wn);
                    os_path = wide;
                }
            }
#else
            os_path = model_path;
#endif
            session_ = std::make_unique<Ort::Session>(GlobalEnv(), os_path.c_str(), opts);
        }
        catch (const Ort::Exception &e)
        {
            std::cerr << "[OnnxEngine] 加载失败 " << model_path << ": " << e.what() << std::endl;
            session_.reset();
            return false;
        }

        Ort::AllocatorWithDefaultOptions allocator;
        input_names_.clear();
        output_names_.clear();
        for (size_t i = 0; i < session_->GetInputCount(); ++i)
            input_names_.emplace_back(session_->GetInputNameAllocated(i, allocator).get());
        for (size_t i = 0; i < session_->GetOutputCount(); ++i)
            output_names_.emplace_back(session_->GetOutputNameAllocated(i, allocator).get());

        std::cout << "[OnnxEngine] " << model_path << "  in=" << input_names_[0]
                  << "  out=" << output_names_[0] << std::endl;
        return true;
    }

    std::vector<float> OnnxEngine::Run(const float *data, const std::vector<int64_t> &shape,
                                       std::vector<int64_t> &out_shape, size_t out_idx)
    {
        out_shape.clear();
        if (!session_)
            return {};

        const int64_t n = ElementCount(shape);
        if (n <= 0)
            return {};

        auto mem_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value input = Ort::Value::CreateTensor<float>(
            mem_info, const_cast<float *>(data), static_cast<size_t>(n), shape.data(), shape.size());

        std::vector<const char *> in_names, out_names;
        in_names.reserve(input_names_.size());
        out_names.reserve(output_names_.size());
        for (auto &s : input_names_)
            in_names.push_back(s.c_str());
        for (auto &s : output_names_)
            out_names.push_back(s.c_str());

        std::vector<Ort::Value> outputs;
        try
        {
            outputs = session_->Run(Ort::RunOptions{nullptr}, in_names.data(), &input, 1,
                                    out_names.data(), out_names.size());
        }
        catch (const Ort::Exception &e)
        {
            std::cerr << "[OnnxEngine] 推理失败: " << e.what() << std::endl;
            return {};
        }

        if (out_idx >= outputs.size())
        {
            std::cerr << "[OnnxEngine] 输出下标越界: " << out_idx << " / " << outputs.size()
                      << std::endl;
            return {};
        }

        auto info = outputs[out_idx].GetTensorTypeAndShapeInfo();
        out_shape = info.GetShape();
        const float *p = outputs[out_idx].GetTensorData<float>();
        return std::vector<float>(p, p + info.GetElementCount());
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
