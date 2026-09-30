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
#include "postprocess_op.h"
#include "onnx_engine.h"

namespace PaddleOCR
{

    class DBDetector
    {
    public:
        DBDetector();

        // num_threads <= 0 表示交给 ORT 自己决定
        void LoadModel(const std::string &model_dir, int num_threads = 0);

        // Run predictor
        void Run(cv::Mat &img, std::vector<std::vector<std::vector<int>>> &boxes,
                 std::vector<double> &times);

        void SetDbParams(double thresh, double box_thresh, double unclip_ratio)
        {
            det_db_thresh_ = thresh;
            det_db_box_thresh_ = box_thresh;
            det_db_unclip_ratio_ = unclip_ratio;
        }

        void SetLimit(int limit_side_len, const std::string &limit_type)
        {
            limit_side_len_ = limit_side_len;
            limit_type_ = limit_type;
        }

        // 归一化后是否交换 B/R 通道再送模型。
        // 默认 true：与旧 cv::dnn 路径一致，且在合成截图集上实测略优
        // （CER 0.879% -> 0.791%）。
        void SetSwapRB(bool v) { swap_rb_ = v; }

        OnnxEngine predictor_; // ONNX Runtime 推理实例

        // 最近一次 Run 实际喂给模型的输入尺寸（用于诊断/统计）
        int LastInputH() const { return last_in_h_; }
        int LastInputW() const { return last_in_w_; }

    private:
        int last_in_h_ = 0;
        int last_in_w_ = 0;
        std::string limit_type_ = "max";
        int limit_side_len_ = 960;

        double det_db_thresh_ = 0.2;
        double det_db_box_thresh_ = 0.45;
        double det_db_unclip_ratio_ = 1.4;
        std::string det_db_score_mode_ = "slow";
        bool use_dilation_ = false;

        bool swap_rb_ = true;

        std::vector<float> mean_ = {0.485f, 0.456f, 0.406f};
        std::vector<float> scale_ = {1 / 0.229f, 1 / 0.224f, 1 / 0.225f};
        bool is_scale_ = true;

        // pre-process
        ResizeImgType0 resize_op_;
        Normalize normalize_op_;
        Permute permute_op_;

        // post-process
        DBPostProcessor post_processor_;
    };

} // namespace PaddleOCR
