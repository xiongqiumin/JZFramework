# ============================================================================
# ONNX Runtime 配置 (可选依赖)
# ============================================================================
# 与 cmake/opencv.cmake 同一套路: 找得到就接上, 找不到就跳过, **不 REQUIRED**
# (没装 ONNX Runtime 不影响任何别的模块)。
#
# 布局 (官方 Windows 预编译包 onnxruntime-win-x64-<ver>.zip 解压后):
#   <JZ_ONNXRUNTIME_DIR>/include/onnxruntime_cxx_api.h / onnxruntime_c_api.h ...   头文件
#   <JZ_ONNXRUNTIME_DIR>/lib/onnxruntime.lib                                       导入库
#   <JZ_ONNXRUNTIME_DIR>/lib/onnxruntime_providers_shared.lib
#   <JZ_ONNXRUNTIME_DIR>/lib/onnxruntime.dll                                       运行库
#   <JZ_ONNXRUNTIME_DIR>/lib/onnxruntime_providers_shared.dll
#   ⚠ 这个包把 **DLL 也放在 lib/** (没有单独的 bin/), 所以拷运行库要从 lib/ 取。
#   <JZ_ONNXRUNTIME_DIR>/VERSION_NUMBER                                            版本号(纯文本)
#
# 覆盖探测: -DJZ_ONNXRUNTIME_DIR=<解压目录>
#
# 用法 (在需要它的 target 上):
#   jz_onnx_enable(<target>)   # 加 JZ_ONNX=1 + 头/库 + 拷 DLL 到输出目录
#     ⚠ 用到它的源文件请用 `#if JZ_ONNX` 守卫, 保证没装时也能编过。
#
# 注: 探测结果用**普通变量**(不用 cache) —— 理由同 cmake/opencv.cmake 文件头:
#     只跑一次由 include_guard 保证; 进 cache 会在改了探测逻辑后残留旧结果。
# ============================================================================

include_guard(GLOBAL)

if(NOT DEFINED JZ_ONNXRUNTIME_DIR)
    # 本机 lib 仓库里的包名带版本号, 用 glob 收 (解压新版不必改这里)
    file(GLOB _jz_ort_candidates
        "C:/Users/7052/Desktop/work/lib/onnxruntime-win-x64-*"
        "C:/onnxruntime*/onnxruntime-win-x64-*")
    list(APPEND _jz_ort_candidates
        "$ENV{ONNXRUNTIME_ROOT}"
        "$ENV{ONNXRUNTIME_DIR}"
        "C:/onnxruntime")                      # 有人直接解到根

    # 有多个版本时取**版本号最大**的那个: 新模型会要求较新的 ORT
    # (实测: PP-OCRv6 的 ONNX 是 IR v10, ORT 1.17 只支持到 IR 9 => 加载失败;
    #  而目录名字典序会把 1.17 排在 1.9 前面这类顺序当不得准)
    set(_jz_ort_best "")
    set(_jz_ort_best_num -1)
    foreach(_c IN LISTS _jz_ort_candidates)
        if(_c AND EXISTS "${_c}/include/onnxruntime_cxx_api.h")
            get_filename_component(_n "${_c}" NAME)
            string(REGEX MATCH "([0-9]+)\\.([0-9]+)" _m "${_n}")
            if(_m)
                math(EXPR _num "${CMAKE_MATCH_1} * 1000 + ${CMAKE_MATCH_2}")
                if(_num GREATER _jz_ort_best_num)
                    set(_jz_ort_best_num ${_num})
                    set(_jz_ort_best "${_c}")
                endif()
            elseif(_jz_ort_best STREQUAL "")
                set(_jz_ort_best "${_c}")   # 名字里没版本号: 兜底
            endif()
        endif()
    endforeach()
    if(_jz_ort_best)
        set(JZ_ONNXRUNTIME_DIR "${_jz_ort_best}" CACHE PATH "ONNX Runtime 解压目录 (含 include/ 与 lib/)")
    endif()
endif()

set(JZ_ONNXRUNTIME_FOUND FALSE)
set(JZ_ONNXRUNTIME_INCLUDE_DIR "")
set(JZ_ONNXRUNTIME_LIBS "")
set(JZ_ONNXRUNTIME_DLLS "")
set(JZ_ONNXRUNTIME_VERSION "")

if(JZ_ONNXRUNTIME_DIR AND EXISTS "${JZ_ONNXRUNTIME_DIR}/include/onnxruntime_cxx_api.h")
    set(_lib "${JZ_ONNXRUNTIME_DIR}/lib")
    foreach(_name onnxruntime onnxruntime_providers_shared)
        if(EXISTS "${_lib}/${_name}.lib")
            list(APPEND JZ_ONNXRUNTIME_LIBS "${_lib}/${_name}.lib")     # 导入库
        endif()
        if(EXISTS "${_lib}/${_name}.dll")
            list(APPEND JZ_ONNXRUNTIME_DLLS "${_lib}/${_name}.dll")     # 运行库(这个包 DLL 也在 lib/)
        endif()
    endforeach()

    if(JZ_ONNXRUNTIME_LIBS)
        set(JZ_ONNXRUNTIME_FOUND TRUE)
        set(JZ_ONNXRUNTIME_INCLUDE_DIR "${JZ_ONNXRUNTIME_DIR}/include")
        if(EXISTS "${JZ_ONNXRUNTIME_DIR}/VERSION_NUMBER")
            file(STRINGS "${JZ_ONNXRUNTIME_DIR}/VERSION_NUMBER" JZ_ONNXRUNTIME_VERSION LIMIT_COUNT 1)
        endif()
        message(STATUS "[onnx] found ${JZ_ONNXRUNTIME_VERSION}: ${JZ_ONNXRUNTIME_DIR}")
    else()
        message(STATUS "[onnx] 目录在但没找到导入库: ${_lib}")
    endif()
else()
    message(STATUS "[onnx] 未找到 ONNX Runtime (要启用: -DJZ_ONNXRUNTIME_DIR=<解压目录>)")
endif()

# 把一个 target 接上 ONNX Runtime: JZ_ONNX=1 + 头/库 + 拷 DLL (找不到则只提示)
function(jz_onnx_enable target)
    if(NOT TARGET ${target})
        message(WARNING "[onnx] jz_onnx_enable: 目标 ${target} 不存在")
        return()
    endif()
    if(NOT JZ_ONNXRUNTIME_FOUND)
        message(STATUS "[onnx] 跳过 ${target}: 未找到 ONNX Runtime")
        return()
    endif()

    # ⚠ 属性式追加: 对 target_xxx 的关键字/plain 两种签名都成立 (同一 target 不能混用两种签名)
    set_property(TARGET ${target} APPEND PROPERTY COMPILE_DEFINITIONS JZ_ONNX=1)
    set_property(TARGET ${target} APPEND PROPERTY INCLUDE_DIRECTORIES ${JZ_ONNXRUNTIME_INCLUDE_DIR})
    set_property(TARGET ${target} APPEND PROPERTY LINK_LIBRARIES ${JZ_ONNXRUNTIME_LIBS})

    # 运行库必须与 exe 同目录 (否则加载即失败)
    foreach(_dll IN LISTS JZ_ONNXRUNTIME_DLLS)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_dll}" "$<TARGET_FILE_DIR:${target}>"
            COMMENT "Copy ONNX Runtime runtime next to ${target}")
    endforeach()

    message(STATUS "[onnx] ${target} <- ${JZ_ONNXRUNTIME_VERSION} (${JZ_ONNXRUNTIME_DIR})")
endfunction()
