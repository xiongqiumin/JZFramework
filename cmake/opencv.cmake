# ============================================================================
# OpenCV 配置 (可选依赖: **只服务 computer use 的像素族兜底**)
# ============================================================================
# 谁在用: JZComputerUse::matchImage / matchColor (见 docs/compute-use-design.md §4.1)。
#   像素族在"运行期寻址"里是**兜底**(正路是布局通道), 所以这里**不 REQUIRED**:
#   找得到就接上, 找不到就跳过 —— 那两个函数返回"未找到", 其余功能一个字都不受影响。
#
# ⚠ 版本: 用 **4.x, 不要 5.x**。
#   OpenCV 5 (2026-08-19 发布) 要求 **C++17**, 且移除了 C API、重构了模块
#   (features2d -> features, ml/gapi 移入 contrib); 而本工程是 CMAKE_CXX_STANDARD 14。
#   我们只用 matchTemplate / cv::Scalar 这类 4/5 完全相同的老接口, 5.0 的新东西
#   (DNN 重写 / ONNX 算子 23%->80% / LLM·VLM) 在这里一点用不上
#   ⇒ 不值得为一个兜底能力把整个模块的标准抬到 17。
#
# 布局 (Windows 官方预编译包 opencv-4.x-windows.exe 解压后):
#   <JZ_OPENCV_DIR>/include/opencv2/opencv.hpp                头文件
#   <JZ_OPENCV_DIR>/x64/<vc>/lib/opencv_world4xx.lib          Release 导入库 (world 单文件)
#   <JZ_OPENCV_DIR>/x64/<vc>/lib/opencv_world4xxd.lib         Debug   导入库
#   <JZ_OPENCV_DIR>/x64/<vc>/bin/opencv_world4xx[d].dll       运行库 (必须与 exe 同目录)
#   注: vc16 = VS2019 工具集(v142); v142/v143 二进制兼容 ⇒ VS2022 可直接用, 不必等 vc17。
# 覆盖探测: -DJZ_OPENCV_DIR=<...>/opencv/build
#
# 用法 (在需要它的 target 上, 例如 code_runtime / 测试):
#   jz_opencv_enable(<target>)   # 加 JZ_CU_OPENCV=1 + 头/库 + 按配置拷运行库
#
# 注: 探测结果一律用**普通变量**而不是 cache —— 本文件只跑一次由 include_guard 保证;
#     用 cache 的话改了本文件里的探测逻辑, 旧结果还会留存(实测踩过: 旧 OpenCV_LIBS 是
#     模块列表, 改成认 world 单库后仍一直 LNK1104)。只有用户可覆盖的 JZ_OPENCV_DIR 进 cache。
# ============================================================================

include_guard(GLOBAL)

if(NOT DEFINED JZ_OPENCV_DIR)
    set(_jz_ocv_candidates
        "C:/Users/7052/Desktop/work/lib/opencv/build"   # 本机 lib 仓库 (opencv-4.12.0-windows.exe 解压到此)
        "C:/opencv/build"                               # 官方安装器默认解压路径
        "C:/opencv"                                     # 有人直接解到根目录
        "$ENV{OPENCV_DIR}"                              # 环境变量给了就用
    )
    foreach(_c IN LISTS _jz_ocv_candidates)
        if(_c AND EXISTS "${_c}/include/opencv2/opencv.hpp")
            set(JZ_OPENCV_DIR "${_c}" CACHE PATH "OpenCV build 目录 (含 include/ 与 x64/<vc>/lib)")
            break()
        endif()
    endforeach()
endif()

set(JZ_OPENCV_FOUND FALSE)
set(JZ_OPENCV_VC_DIR "")
set(JZ_OPENCV_WORLD_REL "")
set(JZ_OPENCV_WORLD_DBG "")
set(JZ_OPENCV_LIBS "")
set(JZ_OPENCV_INCLUDE_DIRS "")
set(JZ_OPENCV_VERSION "")

if(JZ_OPENCV_DIR AND EXISTS "${JZ_OPENCV_DIR}/include/opencv2/opencv.hpp")
    # vc 目录: 先 vc17(VS2022 原生), 退回 vc16/vc15 (v142/v143 二进制兼容)
    foreach(_vc vc17 vc16 vc15)
        if(EXISTS "${JZ_OPENCV_DIR}/x64/${_vc}/lib")
            set(JZ_OPENCV_VC_DIR "${JZ_OPENCV_DIR}/x64/${_vc}")
            break()
        endif()
    endforeach()
endif()

if(JZ_OPENCV_VC_DIR)
    # 官方包自带 OpenCVConfig.cmake => 走它拿版本/头文件目录
    if(EXISTS "${JZ_OPENCV_VC_DIR}/lib/OpenCVConfig.cmake")
        set(OpenCV_DIR "${JZ_OPENCV_VC_DIR}/lib" CACHE PATH "由 cmake/opencv.cmake 设定")
    endif()
    find_package(OpenCV QUIET)

    # ⚠ world 包只有 <opencv_world4xx.lib>, 而 OpenCVConfig 给的 OpenCV_LIBS 是**全部模块名**
    #   (opencv_calib3d/opencv_features2d/...), 那些单独的 .lib 在 world 包里根本不存在
    #   ⇒ 直接链 OpenCV_LIBS 必然 LNK1104。所以优先自己认 world 单库。
    file(GLOB _ocv_world_rel "${JZ_OPENCV_VC_DIR}/lib/opencv_world*[0-9].lib")    # Release (结尾数字)
    file(GLOB _ocv_world_dbg "${JZ_OPENCV_VC_DIR}/lib/opencv_world*[0-9]d.lib")   # Debug   (结尾是 d)
    if(_ocv_world_rel AND _ocv_world_dbg)
        list(GET _ocv_world_rel 0 JZ_OPENCV_WORLD_REL)
        list(GET _ocv_world_dbg 0 JZ_OPENCV_WORLD_DBG)
    else()
        set(JZ_OPENCV_LIBS "${OpenCV_LIBS}")   # 非 world 安装: 用 config 给的模块列表
    endif()

    if(OpenCV_FOUND OR JZ_OPENCV_WORLD_REL)
        set(JZ_OPENCV_FOUND TRUE)
        set(JZ_OPENCV_VERSION "${OpenCV_VERSION}")
        set(JZ_OPENCV_INCLUDE_DIRS "${OpenCV_INCLUDE_DIRS}")
        if(NOT JZ_OPENCV_INCLUDE_DIRS)
            set(JZ_OPENCV_INCLUDE_DIRS "${JZ_OPENCV_DIR}/include")
        endif()
        message(STATUS "[opencv] found ${OpenCV_VERSION}: ${JZ_OPENCV_DIR}")
    else()
        message(STATUS "[opencv] 目录在但 find_package 失败: ${JZ_OPENCV_VC_DIR}")
    endif()
else()
    message(STATUS "[opencv] 未找到 (像素族兜底不可用; 要启用: -DJZ_OPENCV_DIR=<...>/opencv/build)")
endif()

# 把一个 target 接上 OpenCV: JZ_CU_OPENCV=1 + 头/库 + 按配置拷运行库 (找不到则只提示)
function(jz_opencv_enable target)
    if(NOT TARGET ${target})
        message(WARNING "[opencv] jz_opencv_enable: 目标 ${target} 不存在")
        return()
    endif()
    if(NOT JZ_OPENCV_FOUND)
        message(STATUS "[opencv] 跳过 ${target}: 未找到 OpenCV")
        return()
    endif()

    # ⚠ 用**属性式**追加而不是 target_xxx(PRIVATE ...): 本仓库里 target_link_libraries 既有
    #   关键字签名(code_runtime 那种)也有 plain 签名(测试里常见的 t JZFramework),
    #   而 CMake 不允许同一 target 混用两种签名 —— 属性式对两者都成立, 且语义等价 PRIVATE。
    set_property(TARGET ${target} APPEND PROPERTY COMPILE_DEFINITIONS JZ_CU_OPENCV=1)
    set_property(TARGET ${target} APPEND PROPERTY INCLUDE_DIRECTORIES ${JZ_OPENCV_INCLUDE_DIRS})

    # Debug/Release 各一版导入库 => 用生成表达式按配置挑
    if(JZ_OPENCV_WORLD_REL AND JZ_OPENCV_WORLD_DBG)
        set_property(TARGET ${target} APPEND PROPERTY LINK_LIBRARIES
                     "$<IF:$<CONFIG:Debug>,${JZ_OPENCV_WORLD_DBG},${JZ_OPENCV_WORLD_REL}>")
    else()
        set_property(TARGET ${target} APPEND PROPERTY LINK_LIBRARIES ${JZ_OPENCV_LIBS})
    endif()

    # 运行库: 必须与 exe 同目录 (world 包 50MB+, 所以只拷当前配置那一个)
    if(JZ_OPENCV_WORLD_REL AND JZ_OPENCV_WORLD_DBG)
        get_filename_component(_rel_name "${JZ_OPENCV_WORLD_REL}" NAME_WE)   # opencv_world4120
        get_filename_component(_dbg_name "${JZ_OPENCV_WORLD_DBG}" NAME_WE)   # opencv_world4120d
        set(_rel_dll "${JZ_OPENCV_VC_DIR}/bin/${_rel_name}.dll")
        set(_dbg_dll "${JZ_OPENCV_VC_DIR}/bin/${_dbg_name}.dll")
        if(EXISTS "${_rel_dll}" AND EXISTS "${_dbg_dll}")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<IF:$<CONFIG:Debug>,${_dbg_dll},${_rel_dll}>" "$<TARGET_FILE_DIR:${target}>"
                COMMENT "Copy OpenCV runtime next to ${target}")
        endif()
    endif()

    message(STATUS "[opencv] ${target} <- ${JZ_OPENCV_VERSION} (${JZ_OPENCV_VC_DIR})")
endfunction()
