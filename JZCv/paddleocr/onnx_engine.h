// 极简 ONNX Runtime 推理封装（替换原来的 cv::dnn / JZModelEngineOnnx）
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <onnxruntime_cxx_api.h>

namespace PaddleOCR
{

    class OnnxEngine
    {
    public:
        OnnxEngine() = default;
        ~OnnxEngine() = default;
        OnnxEngine(const OnnxEngine &) = delete;
        OnnxEngine &operator=(const OnnxEngine &) = delete;

        // num_threads <= 0 表示交给 ORT 自己决定
        bool LoadModel(const std::string &model_path, int num_threads = 0);

        bool IsLoaded() const { return session_ != nullptr; }

        const std::vector<std::string> &InputNames() const { return input_names_; }
        const std::vector<std::string> &OutputNames() const { return output_names_; }

        // blob 为连续内存的 float 数据，shape 为 NCHW（支持动态维度）。
        // 返回第 out_idx 个输出的全部数据，out_shape 回填其形状。
        std::vector<float> Run(const float *data, const std::vector<int64_t> &shape,
                               std::vector<int64_t> &out_shape, size_t out_idx = 0);

        // 便捷重载：直接用 cv::Mat(CV_32F, {N,C,H,W}) 作为输入
        std::vector<float> Run(const cv::Mat &blob, std::vector<int64_t> &out_shape,
                               size_t out_idx = 0);

    private:
        std::unique_ptr<Ort::Session> session_;
        std::vector<std::string> input_names_;
        std::vector<std::string> output_names_;
    };

} // namespace PaddleOCR
