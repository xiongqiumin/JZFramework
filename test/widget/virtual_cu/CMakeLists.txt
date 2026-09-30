# 整条链路: computer-use -> 虚拟桌面 -> 应用。
# 不建任何真实窗口 => 可以进 ctest。
#
# 注: JZComputerUse 目前只在 code_agent 的 runtime 里编 (它依赖 JZScreenShot 抓屏/编码),
#     这里显式带上这两个源文件 —— 与 binary/code_agent/runtime/CMakeLists.txt 同一套依赖。
add_executable(test_cu_virtual
    main.cpp
    ${JZFRAMEWORK_ROOT_DIR}/JZComputerUse/JZComputerUse.cpp
    ${JZFRAMEWORK_ROOT_DIR}/binary/code_agent/JZScreenShot.cpp)

target_include_directories(test_cu_virtual PRIVATE ${JZFRAMEWORK_ROOT_DIR}/binary/code_agent)
target_link_libraries(test_cu_virtual JZFramework)

add_test(NAME cu_virtual COMMAND test_cu_virtual)
