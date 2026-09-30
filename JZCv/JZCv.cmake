# ============================================================================
# JZCv: 计算机视觉 (当前内容: PP-OCR onnxruntime 管线 + 框架侧封装 JZPaddleOCR)
# ============================================================================
# 依赖 (都必需, 缺任一就不参与构建):
#   OpenCV        —— OCR 内核用它做 resize/cvtColor/warpPerspective/rotate (见 cmake/opencv.cmake)
#   ONNX Runtime  —— 推理 (见 cmake/onnx.cmake)
#
# ⚠ 本模块以**单例模块**形式加入 (jzframework_use_modules(... SINGLE_MODULES JZCv)), 即自己一个
#   target。原因: OCR 内核同步自 PaddleOCR(自研 C++ 版)并**保持原样**, 它是 C++17, 而框架全局是
#   C++14 —— 只有独立 target 才能单独设标准; 顺带也避免 OpenCV/ORT 的头污染核心库。
#
# 注: 公共头只出现 cv::Mat(**先用 OpenCV**); 内核/ORT 类型不外泄 (JZPaddleOCR 走 PIMPL)。
#     调用方要用它, 自己也得 jz_opencv_enable(<target>)。

if(NOT JZ_OPENCV_FOUND OR NOT JZ_ONNXRUNTIME_FOUND)
    message(STATUS "[JZCv] 跳过: 需要 OpenCV + ONNX Runtime (见 cmake/opencv.cmake / cmake/onnx.cmake)")
    return()
endif()

set(JZCv_SOURCES
    JZMat.h
    JZMat.cpp
    JZPaddleOCR.h
    JZPaddleOCR.cpp
    paddleocr/onnx_engine.h      paddleocr/onnx_engine.cpp
    paddleocr/ocr_det.h          paddleocr/ocr_det.cpp
    paddleocr/ocr_cls.h          paddleocr/ocr_cls.cpp
    paddleocr/ocr_rec.h          paddleocr/ocr_rec.cpp
    paddleocr/preprocess_op.h    paddleocr/preprocess_op.cpp
    paddleocr/postprocess_op.h   paddleocr/postprocess_op.cpp
    paddleocr/utility.h          paddleocr/utility.cpp
    paddleocr/clipper.h          paddleocr/clipper.cpp
)
convert_to_abs_paths(JZCv_SOURCES)
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZCv_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")

# ⚠ 单例模块的"链接框架"那一句在 jzframework_use_modules 里是 **PRIVATE**的
#   (target_link_libraries(${s} PRIVATE JZFramework)), 而 PRIVATE 不会把 JZFramework 的
#   **公共 include**(JZCore/JZWidget 各目录) 传给 JZCv 的调用方 ⇒ 调用方 include JZPaddleOCR.h
#   时会找不到 JZGlobal.h。所以这里自己再 PUBLIC 链一次(JZCv 的公共头确实依赖框架类型)。
target_link_libraries(${JZ_MODULE_TARGET} PUBLIC JZFramework)

# OCR 内核是 C++17 (原 PaddleOCR 那份就是); 只影响本模块的 target
set_target_properties(${JZ_MODULE_TARGET} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# 可选依赖接线 (两个 helper 来自 cmake/opencv.cmake / cmake/onnx.cmake)
jz_opencv_enable(${JZ_MODULE_TARGET})
jz_onnx_enable(${JZ_MODULE_TARGET})
