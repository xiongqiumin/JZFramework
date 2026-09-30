# JZCv / PP-OCR 冒烟测试 (合成图 -> 识别)
# 注: 模型不在时程序自己返回 77 => ctest 视为**跳过**(不是失败), 见 main.cpp。
add_executable(test_cv_ocr main.cpp)
target_link_libraries(test_cv_ocr PRIVATE JZCv)

# JZCv 是静态库, 它的 OpenCV/ONNX 依赖会传播过来; 但**运行库 DLL 要拷到输出目录**、
# 且测试自己要用 cv::putText => 显式接一次 (两个 helper 见 cmake/opencv.cmake / cmake/onnx.cmake)
jz_opencv_enable(test_cv_ocr)
jz_onnx_enable(test_cv_ocr)

add_test(NAME cv_ocr COMMAND test_cv_ocr)
set_tests_properties(cv_ocr PROPERTIES SKIP_RETURN_CODE 77)
