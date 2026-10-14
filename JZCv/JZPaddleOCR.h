#ifndef JZ_PADDLE_OCR_H_
#define JZ_PADDLE_OCR_H_

#include <memory>
#include <string>
#include <vector>

#include "JZGlobal.h"
#include <opencv2/core.hpp>   // 公共 API 直接用 cv::Mat —— 先直接用 OpenCV(内部类型仍不外泄)

// 一行 OCR 结果: 文本框(轴对齐包围盒, 原图像素坐标) + 文字 + 置信度
// ⚠ 同 JZMat.h: JZCv 是静态单例模块, 自己的类型不带导出宏
struct JZOCRResult
{
    cv::Rect rect;
    std::string text;
    float score = 0.0f;
};

// ==================== PP-OCR (ONNX Runtime) ====================
// 管线: 检测(DB det) -> 方向分类(cls, **可选**) -> 识别(rec)
// 用途: computer use 的**退化兜底** —— 没有可访问布局时从像素读文字
//       (见 docs/compute-use-design.md §4.1: 像素族只在退化时用, 且必须显式)。
//
// ⚠ 实现内核是从 PaddleOCR(自研 C++ 版) 同步来的 (JZCv/paddleocr/**), **保持其命名空间与
//   代码原样**便于跟上游对照。于是:
//   - 本模块依赖 **OpenCV + ONNX Runtime** (见 cmake/opencv.cmake / cmake/onnx.cmake);
//   - 内核是 C++17 ⇒ JZCv 这个 target 单独设 CXX_STANDARD 17, 不动框架全局的 14;
//   - 内核用**异常**报"模型加载失败"(ocr_det.cpp / ocr_rec.cpp), 这里统一转成 bool。
//
// 模型 (本机 work/lib/PaddleOCR/model 下的现成布局, PP-OCRv6):
//   det : PP-OCRv6_small_det/inference.onnx
//   rec : PP-OCRv6_small_rec/inference.onnx   字典: PP-OCRv6_small_rec/dict.txt
//   cls : **没有** —— 所以 clsOnnx 传空, 不做 180° 纠正; 以后补上模型再传。
class JZPaddleOCR
{
public:
    JZPaddleOCR();
    ~JZPaddleOCR();

    // clsOnnx 传空 = 不做方向分类 (本机没有 cls 模型, 就这样用)
    // ⚠ 内核没有 unload 接口 ⇒ 同一个对象**不要重复 init**(换模型请新建对象)。
    bool init(const std::string& detOnnx,
              const std::string& recOnnx,
              const std::string& dictTxt,
              const std::string& clsOnnx = std::string());

    // 便捷: 按上面注释里那套约定, 从 model 根目录找 (PP-OCRv6 small)
    bool initFromDir(const std::string& modelRoot);

    bool isInit() const;
    bool hasClassifier() const;   // 是否载入了方向分类模型
    void deinit();

    // 识别一帧。img 收 8UC3(BGR, OpenCV 习惯) / 8UC4(RGBA) / 8UC1(灰度), 内部统一成 BGR。
    // 分数 < minScore 的结果丢弃 (默认 0.6, 与 PaddleOCR demo 一致)。
    // 返回按阅读顺序 (上->下, 左->右) 排好。
    std::vector<JZOCRResult> ocr(const cv::Mat& img, float minScore = 0.6f);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // JZ_PADDLE_OCR_H_
