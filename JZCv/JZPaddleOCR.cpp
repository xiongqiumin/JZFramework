#include "JZPaddleOCR.h"

#include "JZLog.h"
#include "paddleocr/ocr_det.h"     // PaddleOCR::DBDetector
#include "paddleocr/ocr_cls.h"     // PaddleOCR::Classifier
#include "paddleocr/ocr_rec.h"     // PaddleOCR::CRNNRecognizer
#include "paddleocr/utility.h"     // Utility::GetRotateCropImage

#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <exception>

// 内核实现在这里可见 (PIMPL: 头文件里不出现 ORT / 内核类型)
struct JZPaddleOCR::Impl
{
    PaddleOCR::DBDetector     det;
    PaddleOCR::Classifier     cls;
    PaddleOCR::CRNNRecognizer rec;

    bool detLoaded = false;
    bool clsLoaded = false;
    bool recLoaded = false;
};

JZPaddleOCR::JZPaddleOCR()
    : m_impl(new Impl())
{
}

JZPaddleOCR::~JZPaddleOCR()
{
    deinit();
}

bool JZPaddleOCR::isInit() const
{
    return m_impl && m_impl->detLoaded && m_impl->recLoaded;
}

bool JZPaddleOCR::hasClassifier() const
{
    return m_impl && m_impl->clsLoaded;
}

void JZPaddleOCR::deinit()
{
    // 内核 (onnx_engine/DBDetector/CRNNRecognizer) 没有 unload 接口 —— 会话随对象析构释放,
    // 所以这里只把状态标记清掉; 想换模型请新建一个 JZPaddleOCR。
    if (m_impl) {
        m_impl->detLoaded = false;
        m_impl->clsLoaded = false;
        m_impl->recLoaded = false;
    }
}

bool JZPaddleOCR::init(const std::string& detOnnx,
                       const std::string& recOnnx,
                       const std::string& dictTxt,
                       const std::string& clsOnnx)
{
    deinit();
    if (!m_impl || detOnnx.empty() || recOnnx.empty() || dictTxt.empty())
        return false;

    try {
        m_impl->det.LoadModel(detOnnx);
        m_impl->detLoaded = true;

        m_impl->rec.LoadLabel(dictTxt);
        m_impl->rec.LoadModel(recOnnx);
        m_impl->recLoaded = true;

        if (!clsOnnx.empty()) {                 // 有 cls 模型才做方向分类
            m_impl->cls.LoadModel(clsOnnx);
            m_impl->clsLoaded = true;
        }
    } catch (const std::exception& e) {
        // 内核靠异常报"加载失败"(见 ocr_det.cpp / ocr_rec.cpp), 这里统一转成 false
        LOG_FMT_E("[JZCv] OCR 模型加载失败: %s", e.what());
        deinit();
        return false;
    }
    return isInit();
}

bool JZPaddleOCR::initFromDir(const std::string& modelRoot)
{
    if (modelRoot.empty())
        return false;
    const char last = modelRoot[modelRoot.size() - 1];
    const std::string base = modelRoot + ((last == '/' || last == '\\') ? "" : "/");

    // 本机 work/lib/PaddleOCR/model 下的现成布局 (PP-OCRv6 small; 无 cls 模型)
    return init(base + "PP-OCRv6_small_det/inference.onnx",
                base + "PP-OCRv6_small_rec/inference.onnx",
                base + "PP-OCRv6_small_rec/dict.txt");
}

std::vector<JZOCRResult> JZPaddleOCR::ocr(const cv::Mat& img, float minScore)
{
    std::vector<JZOCRResult> out;
    if (!isInit() || img.empty())
        return out;

    // ① 统一成 8UC3(BGR): 内核是按 cv::imread 的 BGR 习惯写的
    cv::Mat bgr;
    if (img.depth() != CV_8U) {
        img.convertTo(bgr, CV_8U);              // 顺手归一化深度
    } else if (img.channels() == 4) {
        cv::cvtColor(img, bgr, cv::COLOR_RGBA2BGR);
    } else if (img.channels() == 1) {
        cv::cvtColor(img, bgr, cv::COLOR_GRAY2BGR);
    } else {
        bgr = img.clone();
    }
    if (bgr.empty() || bgr.channels() != 3)
        return out;

    // ② 检测 (DBDetector::Run 收**非 const** Mat)
    std::vector<std::vector<std::vector<int>>> boxes;
    std::vector<double> times;
    m_impl->det.Run(bgr, boxes, times);
    if (boxes.empty())
        return out;

    // ③ 逐框透视裁剪 (任意四边形 -> 摆正的文本条), 同时记下它的轴对齐包围盒
    std::vector<cv::Mat> crops;
    crops.reserve(boxes.size());
    out.reserve(boxes.size());
    for (auto& box : boxes) {
        if (box.size() < 4)
            continue;
        crops.push_back(PaddleOCR::Utility::GetRotateCropImage(bgr, box));

        int xs[4] = { box[0][0], box[1][0], box[2][0], box[3][0] };
        int ys[4] = { box[0][1], box[1][1], box[2][1], box[3][1] };
        const int l = *std::min_element(xs, xs + 4);
        const int r = *std::max_element(xs, xs + 4);
        const int t = *std::min_element(ys, ys + 4);
        const int b = *std::max_element(ys, ys + 4);

        JZOCRResult res;
        res.rect = cv::Rect(l, t, r - l, b - t);
        out.push_back(res);
    }
    if (crops.empty())
        return out;

    // ④ 方向分类 (可选): label 为奇数且够自信 => 180° 转正
    if (m_impl->clsLoaded) {
        std::vector<int> labels;
        std::vector<float> scores;
        std::vector<double> clsTimes;
        m_impl->cls.Run(crops, labels, scores, clsTimes);
        const float thresh = 0.9f;
        for (size_t i = 0; i < crops.size() && i < labels.size() && i < scores.size(); ++i) {
            if (labels[i] % 2 == 1 && scores[i] > thresh)
                cv::rotate(crops[i], crops[i], 1);
        }
    }

    // ⑤ 识别
    std::vector<std::string> texts;
    std::vector<float> recScores;
    std::vector<double> recTimes;
    m_impl->rec.Run(crops, texts, recScores, recTimes);

    const size_t n = std::min(out.size(), texts.size());
    for (size_t i = 0; i < n; ++i) {
        out[i].text = texts[i];
        out[i].score = (i < recScores.size()) ? recScores[i] : 0.0f;
    }

    // ⑥ 过滤低分 + 排成阅读顺序 (上->下, 同行左->右)
    std::vector<JZOCRResult> keep;
    keep.reserve(out.size());
    for (auto& r : out) {
        if (r.score >= minScore)
            keep.push_back(r);
    }
    std::stable_sort(keep.begin(), keep.end(), [](const JZOCRResult& a, const JZOCRResult& b) {
        // 同一行(纵向重叠超过半行高)按 x 排; 否则按 y 排
        const int overlap = std::min(a.rect.y + a.rect.height, b.rect.y + b.rect.height)
                          - std::max(a.rect.y, b.rect.y);
        if (overlap > std::min(a.rect.height, b.rect.height) / 2)
            return a.rect.x < b.rect.x;
        return a.rect.y < b.rect.y;
    });
    return keep;
}
