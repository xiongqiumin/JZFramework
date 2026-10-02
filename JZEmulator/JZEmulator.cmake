# ============================================================================
# JZEmulator: Game Boy Advance 画面控件（mGBA 核心 + JZWidget 显示/键盘）
# ============================================================================
# 依赖:
#   JZWidget          —— 控件/绘制/键盘/定时器（本模块就是一套控件）
#   mgba_host.dll     —— **外部**宿主 DLL（C ABI），来自 E:/work/simulator 那套工程：
#                        mGBA 源码用 MinGW-w64(gcc 8.1) 编成 libmgba.a，再由它的
#                        host/mgba_host.c 封成 C ABI DLL。本框架是 MSVC，所以走
#                        "跨 C ABI 动态加载"而不是链库（理由见 JZEmulatorCore.h）。
#
# ⚠ 它是**单例模块**（jzframework_use_modules(... SINGLE_MODULES JZEmulator)）：自己一个
#   target，免得把"第三方 DLL 的动态加载 + 帧缓冲"这些东西掺进核心库。
#
# 用法（在需要它的可执行目标上）:
#   if(HAS_JZEmulator)
#       jz_emulator_enable(<target>)        # 链接 + 头 + 把 mgba_host.dll 与其依赖拷到 exe 边上
#   endif()
#
# 覆盖 DLL 位置: -DJZ_MGBA_DIR=<含 mgba_host.dll 的目录>
# ============================================================================

if(NOT HAS_JZWidget)
    message(STATUS "[JZEmulator] 跳过: 需要 JZWidget（Windows 才有）")
    return()
endif()

set(JZEmulator_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}")

if(NOT DEFINED JZ_MGBA_DIR)
    # E:/work/simulator 那套工程 build 出来的地方（它的 host/CMakeLists.txt 输出目录）
    set(JZ_MGBA_DIR "E:/work/simulator/host/build" CACHE PATH
        "mgba_host.dll 所在目录（含 libgcc_s_seh-1.dll 等 MinGW 运行库）")
endif()

# 把一个可执行目标接上 JZEmulator：头/链接 + 把宿主 DLL 与它依赖的 MinGW 运行库
# 拷到 exe 边上。找不到 DLL 时只提示、不报错（调用方应先判断 HAS_JZEmulator）。
function(jz_emulator_enable target)
    if(NOT TARGET ${target})
        message(WARNING "[JZEmulator] jz_emulator_enable: 目标 ${target} 不存在")
        return()
    endif()

    # ⚠ 用**属性式**追加（而不是 target_xxx(PRIVATE ...)）：本仓库里同一个 target
    #   既有关键字签名也有 plain 签名，混用会被 CMake 拒绝；属性式对两者都成立。
    set_property(TARGET ${target} APPEND PROPERTY INCLUDE_DIRECTORIES "${JZEmulator_SOURCE_DIR}")
    set_property(TARGET ${target} APPEND PROPERTY COMPILE_DEFINITIONS JZ_HAS_EMULATOR=1)
    if(NOT ${target} STREQUAL "JZEmulator")
        set_property(TARGET ${target} APPEND PROPERTY LINK_LIBRARIES JZEmulator)
    endif()

    # 静态库（如 jz_virtual_process：JZEngineRuntime 编在它里面）只接线头/宏/链接；
    # 拷 DLL 那一步交给最终 exe 的 jz_emulator_enable（静态库没有"运行时目录"）。
    # 注：静态库的 PRIVATE 链接依赖会被 CMake 以 $<LINK_ONLY:...> 记进 INTERFACE,
    #     所以最终 exe 链上它时也会链 JZEmulator, 不必重复 enable。
    get_target_property(_jz_emu_type ${target} TYPE)
    if(_jz_emu_type STREQUAL "STATIC_LIBRARY")
        message(STATUS "[JZEmulator] ${target}: 静态库 —— 只接线（拷 DLL 交给最终 exe）")
        return()
    endif()

    if(NOT EXISTS "${JZ_MGBA_DIR}/mgba_host.dll")
        message(STATUS "[JZEmulator] ${target}: 没找到 ${JZ_MGBA_DIR}/mgba_host.dll，"
                       "运行时会加载失败（用 -DJZ_MGBA_DIR=... 指定）")
        return()
    endif()

    # ⚠ 这三个 MinGW 运行库必须与 mgba_host.dll **同目录**：加载它用的是
    #   LoadLibraryEx(..., LOAD_WITH_ALTERED_SEARCH_PATH)，依赖会从该 DLL 自己所在的
    #   目录找（这样不必改 PATH）。见 JZEmulatorCore::load()。
    foreach(_f mgba_host.dll libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
        if(EXISTS "${JZ_MGBA_DIR}/${_f}")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${JZ_MGBA_DIR}/${_f}" "$<TARGET_FILE_DIR:${target}>"
                COMMENT "Copy ${_f} next to ${target}")
        else()
            message(STATUS "[JZEmulator] 缺 ${JZ_MGBA_DIR}/${_f}（能编，但运行时可能加载失败）")
        endif()
    endforeach()

    message(STATUS "[JZEmulator] ${target} <- mgba_host.dll (${JZ_MGBA_DIR})")
endfunction()

set(JZEmulator_SOURCES
    JZEmulatorCore.h  JZEmulatorCore.cpp
    JZEmulatorView.h  JZEmulatorView.cpp
    JZEmulatorViewJs.h JZEmulatorViewJs.cpp   # 把控件挂到 JS 类层(见该文件头的说明)
)
convert_to_abs_paths(JZEmulator_SOURCES)
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZEmulator_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${JZEmulator_SOURCE_DIR}")
target_link_libraries(${JZ_MODULE_TARGET} PUBLIC JZFramework)
