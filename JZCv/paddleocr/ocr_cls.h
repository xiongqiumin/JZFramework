// Copyright (c) 2020 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <opencv2/opencv.hpp>
#include "preprocess_op.h"
#include "utility.h"
#include "onnx_engine.h"

namespace PaddleOCR
{

    class Classifier
    {
    public:
        explicit Classifier();
        double cls_thresh = 0.9;

        // num_threads <= 0 表示交给 ORT 自己决定
        void LoadModel(const std::string &model_dir, int num_threads = 0);

        void Run(std::vector<cv::Mat> img_list, std::vector<int> &cls_labels,
                 std::vector<float> &cls_scores, std::vector<double> &times);

        OnnxEngine predictor_; // ONNX Runtime 推理实例

    private:
        std::vector<float> mean_ = {0.5f, 0.5f, 0.5f};
        std::vector<float> scale_ = {1 / 0.5f, 1 / 0.5f, 1 / 0.5f};
        bool is_scale_ = true;

        int cls_batch_num_ = 1;
        std::vector<int> cls_image_shape_ = {3, 48, 192};
        // pre-process
        ClsResizeImg resize_op_;
        Normalize normalize_op_;
        PermuteBatch permute_op_;

    }; // class Classifier

} // namespace PaddleOCR
