// ==================== JZCv / PP-OCR 冒烟测试 ====================
// 验证"模型能载入 + 真能识别出字"这条链, 用**合成图**(cv::putText)做输入 ——
// 不依赖任何外部图片素材, 也不碰屏幕。
//
// 模型目录: 环境变量 JZCV_OCR_MODEL, 否则用本机默认路径(见下)。
// 模型不在 / 载入失败 => 返回 77 (= ctest SKIP, 与 binary/agent_cli 的 icon_finder.py 同一约定),
// 这样换台机器/CI 上没放模型时是"跳过"而不是"失败"。
#include <cstdio>
#include <cstdlib>
#include <string>

#include "JZPaddleOCR.h"
#include <opencv2/imgproc.hpp>
#include <opencv2/core/utils/logger.hpp>

static int g_fail = 0;

static void check(bool ok, const char* msg)
{
    if (ok) {
        printf("  [ ok ] %s\n", msg);
    } else {
        printf("  [FAIL] %s\n", msg);
        ++g_fail;
    }
}

int main(int argc, char* argv[])
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    // 关掉 OpenCV 的 INFO 噪音(它在找 TBB/OpenMP 并行插件, 找不到只是提示) —— ctest 输出才干净
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_SILENT);

    std::string modelRoot;
    if (const char* env = std::getenv("JZCV_OCR_MODEL"))
        modelRoot = env;
    else
        modelRoot = "C:/Users/7052/Desktop/work/lib/PaddleOCR/model";   // 本机 PP-OCRv6 small

    JZPaddleOCR ocr;
    if (!ocr.initFromDir(modelRoot)) {
        printf("[cv] skip: OCR 模型载入失败 (%s)\n", modelRoot.c_str());
        printf("     设 JZCV_OCR_MODEL=<model 根目录> 可指定; 返回 77 = ctest 跳过\n");
        return 77;
    }

    printf("[cv] OCR 模型载入成功 (cls=%s)\n", ocr.hasClassifier() ? "有" : "无(不做方向纠正)");

    // 合成一张"文档"图: 白底黑字, 字号放大到 det 容易检出的尺度
    cv::Mat img(220, 640, CV_8UC3, cv::Scalar(255, 255, 255));
    cv::putText(img, "Hello 12345", cv::Point(24, 130),
                cv::FONT_HERSHEY_SIMPLEX, 2.4, cv::Scalar(0, 0, 0), 5, cv::LINE_AA);

    const std::vector<JZOCRResult> res = ocr.ocr(img, 0.5f);

    std::string all;
    for (const auto& r : res)
        all += r.text + "|";

    printf("[cv] 结果 %d 条: %s\n", (int)res.size(), all.c_str());

    check(!res.empty(), "检测到文本框");
    check(all.find("12345") != std::string::npos, "识别出 \"12345\"");
    check(all.find("Hello") != std::string::npos || all.find("Hel") != std::string::npos,
          "识别出 \"Hello\" 段");

    // 空图不该崩, 也不该吐结果
    {
        cv::Mat empty;
        check(ocr.ocr(empty).empty(), "空图返回空结果 (不崩)");
    }

    // RGBA 输入也要能收 (CU 那边是 RGBA8888)
    {
        cv::Mat rgba;
        cv::cvtColor(img, rgba, cv::COLOR_BGR2RGBA);
        const std::vector<JZOCRResult> r2 = ocr.ocr(rgba, 0.5f);
        check(!r2.empty(), "8UC4(RGBA) 输入同样能识别");
    }

    printf(g_fail == 0 ? "[cv] 全部通过\n" : "[cv] 失败 %d 项\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
