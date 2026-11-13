# 整条链路: computer-use -> 虚拟桌面 -> 应用。
# 不建任何真实窗口 => 可以进 ctest。
#
# 注: JZComputerUse 目前只在 code_agent 的 runtime 里编 (它依赖 JZScreenShot 抓屏/编码),
#     这里显式带上这两个源文件 —— 与 binary/code_agent/runtime/CMakeLists.txt 同一套依赖。
add_executable(test_cu_virtual
    main.cpp
    ${JZFRAMEWORK_ROOT_DIR}/JZComputerUse/JZComputerUse.cpp
    ${JZFRAMEWORK_ROOT_DIR}/binary/agent_cli/desktop/JZScreenShot.cpp)

target_include_directories(test_cu_virtual PRIVATE ${JZFRAMEWORK_ROOT_DIR}/binary/agent_cli/desktop)
target_link_libraries(test_cu_virtual JZFramework)
# 虚拟桌面 (JZComputerUse::setVirtualDesktop 用的就是它)
target_link_libraries(test_cu_virtual jz_virtual_device)

add_test(NAME cu_virtual COMMAND test_cu_virtual)

# 可选: 接上 OpenCV (像素族兜底 matchImage/matchColor)。没装就自动跳过, 不影响本测试。
jz_opencv_enable(test_cu_virtual)

# 有 JZCv(OpenCV + ONNX Runtime 齐)时再开 OCR 兜底 —— 本测试据此验证
# match({kind:'ocr', text}) 真能在虚拟桌面上把文字找出来并点中。
if(HAS_JZCv)
    set_property(TARGET test_cu_virtual APPEND PROPERTY LINK_LIBRARIES JZCv)   # 兼容 plain 签名
    target_compile_definitions(test_cu_virtual PRIVATE JZ_CU_OCR=1)
    jz_onnx_enable(test_cu_virtual)
endif()
