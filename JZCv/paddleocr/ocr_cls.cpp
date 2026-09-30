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

#include "ocr_cls.h"

using namespace cv;

namespace PaddleOCR
{

    Classifier::Classifier()
    {
    }

    void Classifier::Run(std::vector<cv::Mat> img_list,
                         std::vector<int> &cls_labels,
                         std::vector<float> &cls_scores,
                         std::vector<double> &times)
    {
        std::chrono::duration<float> preprocess_diff = std::chrono::duration<float>::zero();
        std::chrono::duration<float> inference_diff = std::chrono::duration<float>::zero();
        std::chrono::duration<float> postprocess_diff = std::chrono::duration<float>::zero();

        int img_num = img_list.size();
        cls_labels.resize(img_num, 0);
        cls_scores.resize(img_num, 0.f);

        const int imgH = cls_image_shape_[1];
        const int imgW = cls_image_shape_[2];

        for (int beg_img_no = 0; beg_img_no < img_num;
             beg_img_no += this->cls_batch_num_)
        {
            auto preprocess_start = std::chrono::steady_clock::now();
            int end_img_no = std::min(img_num, beg_img_no + this->cls_batch_num_);
            int batch_num = end_img_no - beg_img_no;

            std::vector<cv::Mat> norm_img_batch;
            for (int ino = beg_img_no; ino < end_img_no; ino++)
            {
                cv::Mat srcimg;
                img_list[ino].copyTo(srcimg);
                cv::Mat resize_img;
                this->resize_op_.Run(srcimg, resize_img, false, cls_image_shape_);

                this->normalize_op_.Run(&resize_img, this->mean_, this->scale_,
                                        this->is_scale_);
                if (resize_img.cols < imgW)
                {
                    cv::copyMakeBorder(resize_img, resize_img, 0, 0, 0,
                                       imgW - resize_img.cols,
                                       cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
                }
                norm_img_batch.push_back(resize_img);
            }
            std::vector<float> input(
                static_cast<size_t>(batch_num) * cls_image_shape_[0] * imgH * imgW, 0.0f);
            this->permute_op_.Run(norm_img_batch, input.data());
            auto preprocess_end = std::chrono::steady_clock::now();
            preprocess_diff += preprocess_end - preprocess_start;

            // inference.
            auto inference_start = std::chrono::steady_clock::now();

            std::vector<int64_t> in_shape = {batch_num, 3, imgH, imgW};
            std::vector<int64_t> out_shape;
            std::vector<float> out = predictor_.Run(input.data(), in_shape, out_shape);

            auto inference_end = std::chrono::steady_clock::now();
            inference_diff += inference_end - inference_start;

            if (out.empty() || out_shape.size() < 2)
                continue;

            const int rows = static_cast<int>(out_shape[0]);
            const int cols = static_cast<int>(out_shape[1]);
            auto postprocess_start = std::chrono::steady_clock::now();
            for (int batch_idx = 0; batch_idx < rows; batch_idx++)
            {
                const float *p = out.data() + static_cast<size_t>(batch_idx) * cols;
                int label = int(Utility::argmax(p, p + cols));
                float score = float(*std::max_element(p, p + cols));
                cls_labels[beg_img_no + batch_idx] = label;
                cls_scores[beg_img_no + batch_idx] = score;
            }
            auto postprocess_end = std::chrono::steady_clock::now();
            postprocess_diff += postprocess_end - postprocess_start;
        }
        times.push_back(double(preprocess_diff.count() * 1000));
        times.push_back(double(inference_diff.count() * 1000));
        times.push_back(double(postprocess_diff.count() * 1000));
    }

    void Classifier::LoadModel(const std::string &model_dir, int num_threads)
    {
        if (!predictor_.LoadModel(model_dir, num_threads))
            throw std::runtime_error("Classifier: 无法加载模型 " + model_dir);
    }

} // namespace PaddleOCR
