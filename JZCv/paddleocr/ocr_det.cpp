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

#include "ocr_det.h"

#include <cstring>

using namespace cv;

namespace PaddleOCR
{
    DBDetector::DBDetector()
    {
    }

    void DBDetector::LoadModel(const std::string &model_dir, int num_threads)
    {
        if (!predictor_.LoadModel(model_dir, num_threads))
            throw std::runtime_error("DBDetector: 无法加载模型 " + model_dir);
    }

    void DBDetector::Run(cv::Mat &img,
                         std::vector<std::vector<std::vector<int>>> &boxes,
                         std::vector<double> &times)
    {
        float ratio_h{};
        float ratio_w{};

        cv::Mat srcimg;
        cv::Mat resize_img;
        img.copyTo(srcimg);

        auto preprocess_start = std::chrono::steady_clock::now();
        this->resize_op_.Run(img, resize_img, this->limit_type_,
                             this->limit_side_len_, ratio_h, ratio_w,
                             false);

        // BGR 浮点、已按 ImageNet 均值方差归一化，尺寸为 HWC
        this->normalize_op_.Run(&resize_img, this->mean_, this->scale_,
                                this->is_scale_);

        const int imgH = resize_img.rows;
        const int imgW = resize_img.cols;
        last_in_h_ = imgH;
        last_in_w_ = imgW;

        // HWC -> CHW（OpenCV 只做图像处理，布局转换自己做）
        std::vector<cv::Mat> channels(3);
        cv::split(resize_img, channels);
        const int map[3] = {swap_rb_ ? 2 : 0, 1, swap_rb_ ? 0 : 2};

        std::vector<float> input(static_cast<size_t>(3) * imgH * imgW, 0.f);
        for (int c = 0; c < 3; ++c)
        {
            std::memcpy(input.data() + static_cast<size_t>(c) * imgH * imgW,
                        channels[map[c]].ptr<float>(),
                        static_cast<size_t>(imgH) * imgW * sizeof(float));
        }

        auto preprocess_end = std::chrono::steady_clock::now();

        // Inference.
        auto inference_start = std::chrono::steady_clock::now();

        std::vector<int64_t> in_shape = {1, 3, imgH, imgW};
        std::vector<int64_t> out_shape;
        std::vector<float> out = predictor_.Run(input.data(), in_shape, out_shape);
        if (out.empty() || out_shape.size() < 4)
        {
            times.push_back(0);
            times.push_back(0);
            times.push_back(0);
            return;
        }

        auto inference_end = std::chrono::steady_clock::now();

        auto postprocess_start = std::chrono::steady_clock::now();
        const int n2 = static_cast<int>(out_shape[2]);
        const int n3 = static_cast<int>(out_shape[3]);
        const int n = n2 * n3;

        float *out_data = out.data();
        std::vector<unsigned char> cbuf(n, ' ');

        for (int i = 0; i < n; i++)
        {
            cbuf[i] = (unsigned char)((out_data[i]) * 255);
        }

        cv::Mat cbuf_map(n2, n3, CV_8UC1, (unsigned char *)cbuf.data());
        cv::Mat pred_map(n2, n3, CV_32F, (float *)out_data);

        const double threshold = this->det_db_thresh_ * 255;
        const double maxvalue = 255;
        cv::Mat bit_map;
        cv::threshold(cbuf_map, bit_map, threshold, maxvalue, cv::THRESH_BINARY);
        if (this->use_dilation_)
        {
            cv::Mat dila_ele =
                cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2, 2));
            cv::dilate(bit_map, bit_map, dila_ele);
        }

        boxes = post_processor_.BoxesFromBitmap(
            pred_map, bit_map, this->det_db_box_thresh_, this->det_db_unclip_ratio_,
            this->det_db_score_mode_);

        boxes = post_processor_.FilterTagDetRes(boxes, ratio_h, ratio_w, srcimg);
        auto postprocess_end = std::chrono::steady_clock::now();

        std::chrono::duration<float> preprocess_diff =
            preprocess_end - preprocess_start;
        times.push_back(double(preprocess_diff.count() * 1000));
        std::chrono::duration<float> inference_diff = inference_end - inference_start;
        times.push_back(double(inference_diff.count() * 1000));
        std::chrono::duration<float> postprocess_diff =
            postprocess_end - postprocess_start;
        times.push_back(double(postprocess_diff.count() * 1000));
    }

} // namespace PaddleOCR
