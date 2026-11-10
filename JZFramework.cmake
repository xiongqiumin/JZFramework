cmake_minimum_required(VERSION 3.12)

# 防止文件被重复 include
include_guard(GLOBAL)

# ==================== Python 解释器 ====================
# 部分代码生成脚本 (jz_resources.py / jz_proto_json.py) 依赖 Python。
# 使用 CMake 标准 FindPython3 模块探测解释器 (兼容 python3 / python / py -3 launcher)。
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    set(JZ_PYTHON_EXECUTABLE "${Python3_EXECUTABLE}")
else()
    message(FATAL_ERROR "[JZFramework] Python3 interpreter not found; code generation steps requiring python will fail.")
endif()

# ==================== 静态/动态库切换 ====================
# 默认: **全平台都编动态库**(Windows=DLL, Linux/macOS=.so/.dylib)。
#   * 消费方也需定义 JZFRAMEWORK_DYNAMIC ⇒ 这里用 PUBLIC 自动传递(拿到 dllimport);
#   * 本框架作为 SHARED 库构建, CMake 自动定义 JZFramework_EXPORTS,
#     配合 JZGlobal.h 中的 JZFRAMEWORK_API 完成 dllexport / dllimport。
#   * 回静态库: -DJZFRAMEWORK_DYNAMIC=OFF (或直接改这里的默认值)。
#
# ⚠ 非 Windows 上切动态库的注意点:
#   1) JZFRAMEWORK_API 在非 Windows 侧通常展开为空(见 JZGlobal.h), 所以切 .so 后
#      符号默认全部可见 —— 不依赖 WINDOWS_EXPORT_ALL_SYMBOLS 那套。
#   2) 运行期需要能让 exe 找到 .so: 下面用 RPATH(BUILD_RPATH/INSTALL_RPATH)解决, exe
#      在 build 树里直接跑即可, 无需再设 LD_LIBRARY_PATH。
option(JZFRAMEWORK_DYNAMIC
       "Build JZFramework as a shared library (DLL/.so/.dylib). 默认: 全平台 ON(动态)."
       ON)

if(JZFRAMEWORK_DYNAMIC)
    add_library(JZFramework SHARED "")
    target_compile_definitions(JZFramework PUBLIC JZFRAMEWORK_DYNAMIC)

    # 导出面 = **只认显式导出标记**(2026-11-10 定案: 不再用 WINDOWS_EXPORT_ALL_SYMBOLS)。
    #   曾开 WINDOWS_EXPORT_ALL_SYMBOLS(CMake 扫 .obj 生成 .def, 自动导出全部函数符号),
    #   好处是第三方 C 符号(JS_* / mg_*)和模板显式特化不用逐个标注; 代价:
    #     * vftable(??_7*)这类**数据符号**与类上的 dllexport 重复 ⇒ 每个多态类一条 LNK4197;
    #     * 导出面不可控 —— 内部实现符号全进 DLL 导出表。
    #   关掉之后的约定:
    #     * 框架自己的类 / 自由函数 / 模板显式特化: 一律带 JZFRAMEWORK_API(漏了使用方 LNK2019);
    #       ⚠ 数据符号(全局变量, 如 jzApp)在**使用方**也必须能看到 dllimport, 所以只能靠头文件里
    #       的 JZFRAMEWORK_API, 不能靠 .def。
    #     * jsoncpp(vendored): 下面两条 JSON_DLL / JSON_DLL_BUILD 负责;
    #     * quickjs(JS_*): 用 quickjs 自己的 BUILDING_QJS_SHARED / USING_QJS_SHARED(见下);
    #     * mongoose(mg_*): **不导出**(它是 JZNet 的内部实现)—— 需要直连 C API 的测试自带
    #       一份 mongoose.c, 见 test/net/CMakeLists.txt 的 test_mg* 与那里的说明。

    # 注: 导出类带 std::string/std::vector 成员会刷 C4251/C4275(编 DLL 与使用方都刷)。
    #     本工程已**全局**忽略这两条 —— 见根 CMakeLists.txt 的 MSVC 段(/wd4251 /wd4275),
    #     那里也写了前提: 必须同一套工具集 + 同一动态 CRT(/MD) + 同配置重编。
    #     第三方只拿 DLL + 头、用别的工具集编译时, 那条警告指出的 ABI 风险是真的(那时要源码级重编)。
    # jsoncpp(vendored 1.9.7, 编在 JZJson 模块里, 是**第三方头**)自带导出开关:
    #   建 DLL 侧 JSON_DLL_BUILD -> JSON_API = dllexport; 用方 JSON_DLL -> dllimport。
    # 少了它, 任何 exe 只要直接碰 Json::Value(main.cpp/工具/评测都在碰)就 LNK2019。
    target_compile_definitions(JZFramework PUBLIC JSON_DLL)
    target_compile_definitions(JZFramework PRIVATE JSON_DLL_BUILD)

    # quickjs(源码编进 DLL)自带导出开关: quickjs.h 里的 JS_EXTERN 由
    #   BUILDING_QJS_SHARED -> dllexport(建库侧) / USING_QJS_SHARED -> dllimport(使用方) 决定。
    # 用 INTERFACE 而不是 PUBLIC: 建库侧**只**看到 BUILDING_QJS_SHARED。
    # (两者同时可见时 quickjs.h 靠 #if/#elif 的先后也能选对, 但那是隐式依赖, 别赌。)
    # 少了 BUILDING_QJS_SHARED: JS_* 不进导出表 -> 直接调 quickjs C API 的测试/test_jsengine LNK2019。
    target_compile_definitions(JZFramework PRIVATE BUILDING_QJS_SHARED)
    target_compile_definitions(JZFramework INTERFACE USING_QJS_SHARED)

    # 非 Windows: 给 .so/.dylib 设好输出位置与 RPATH, 让 build 树里的 exe 直接能跑。
    #   * 不设 VERSION/SOVERSION ⇒ 只产出**一个裸 libJZFramework.so**, 不生成
    #     libJZFramework.so.<ver> 那串实体文件 + 软链(本工程直接跑 build 树, 不做 ABI 版本共存)。
    #   * RPATH: 以 **exe 同目录($ORIGIN)** 为准 —— 配合下面 POST_BUILD 把 .so 复制到各 exe
    #     目录, exe 在哪儿都能就地找到库(整个 build 树可整体挪走)。$ORIGIN/..、$ORIGIN/../..
    #     作为兜底, 覆盖"库仍在 <build>/ 根"的情形。
    if(NOT WIN32)
        set_target_properties(JZFramework PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
        set(CMAKE_BUILD_RPATH "$ORIGIN" "$ORIGIN/.." "$ORIGIN/../.." "${CMAKE_BINARY_DIR}")
        set(CMAKE_INSTALL_RPATH "$ORIGIN" "$ORIGIN/.." "$ORIGIN/../..")
    endif()
else()
    add_library(JZFramework STATIC "")
endif()

# ---------------------------------------------------------------------------
# 动态库的运行期落点: 把 JZFramework 运行时(DLL/.so/.dylib)**复制到各 exe 目录**。
#   * Windows: 没有 RPATH 那套 —— exe 旁边找不到 JZFramework.dll 会直接以 0xC0000135
#     起不来(连构建期工具 jz_uic 都会失败), 复制是**唯一**可行办法。
#   * 非 Windows: 上面的 RPATH 已经能让 exe 找到库($ORIGIN 等), 但复制一份到 exe 同目录
#     仍然有价值 —— 单个 exe 目录可自包含地拷走运行, 且有 $ORIGIN 直命中(不经 RUNPATH 兜底)。
#
# 本工程的 exe 只有两个落点(见根 CMakeLists 的 JZ_TEST_BIN_DIR 与 binary/CMakeLists.txt
# 末尾的发布清单), 所以把库复制到这两个目录就够, 不必逐个 exe 挂 POST_BUILD:
#   · <build>/test/bin/<配置>    —— 测试 / 评测 / 开发工具
#   · <build>/binary/bin/<配置>  —— 发布 exe
# ⚠ add_custom_command(TARGET ...) 必须在**创建该 target 的目录**里调用, 这里正是根目录作用域。
# ⚠ 目录此时可能还没建(exe 目标在后面的 add_subdirectory 里), 所以先 make_directory。
# ---------------------------------------------------------------------------
if(JZFRAMEWORK_DYNAMIC)
    set(_jz_shared_dest_dirs
        "${CMAKE_BINARY_DIR}/test/bin/$<CONFIG>"
        "${CMAKE_BINARY_DIR}/binary/bin/$<CONFIG>")
    foreach(_dir ${_jz_shared_dest_dirs})
        add_custom_command(TARGET JZFramework POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:JZFramework>" "${_dir}"
            COMMENT "Copy JZFramework runtime into ${_dir}")
    endforeach()
    unset(_jz_shared_dest_dirs)
endif()

# ==================== 模块依赖关系表 ====================
# 格式: set(MODULE_DEPENDS_<模块名> "依赖模块1;依赖模块2")
set(MODULE_DEPENDS_JZCore "")
set(MODULE_DEPENDS_JZJson "JZCore")
set(MODULE_DEPENDS_JZNet "JZCore")
set(MODULE_DEPENDS_JZFrp "JZNet")
set(MODULE_DEPENDS_JZBle "JZCore")
set(MODULE_DEPENDS_JZWifi "JZCore")
set(MODULE_DEPENDS_JZSerialPort "JZCore")
set(MODULE_DEPENDS_JZGpio "JZCore")
set(MODULE_DEPENDS_JZSensor "JZCore")
set(MODULE_DEPENDS_JZCoroutine "JZCore;JZSerialPort")
set(MODULE_DEPENDS_JZAgent "JZCore;JZNet;JZJson")
set(MODULE_DEPENDS_JZWebView "JZCore")
set(MODULE_DEPENDS_JZMcpServer "JZCore;JZJson;JZHttpServer;JZNet")
set(MODULE_DEPENDS_JZWidget "JZCore;JZJson")
# JZGame 暂不进默认构建 (见根 CMakeLists 的同名注释); 启用模块时这行也要取消注释
# set(MODULE_DEPENDS_JZGame "JZWidget")
set(MODULE_DEPENDS_JZSimulator "JZBle;JZHttpServer;JZJson;JZGpio")
set(MODULE_DEPENDS_JZCamera "JZCore")
set(MODULE_DEPENDS_JZCrashReport "JZCore;JZNet")
set(MODULE_DEPENDS_JZModbus "JZSerialPort;JZNet")
set(MODULE_DEPENDS_JZOTA "JZCore;JZNet;JZJson")
set(MODULE_DEPENDS_JZCompress "JZCore")
set(MODULE_DEPENDS_JZHttpServer "JZNet;JZCompress")
set(MODULE_DEPENDS_JZDatabase "JZCore")
set(MODULE_DEPENDS_JZXml "JZCore")
set(MODULE_DEPENDS_JZDocument "JZCore;JZJson;JZXml;JZCompress")
set(MODULE_DEPENDS_JZJSEngine "JZNet;JZJson;JZHttpServer;JZDatabase;JZModbus;JZXml")

set(JZFRAMEWORK_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}")
message(STATUS "JZFramework root directory: ${JZFRAMEWORK_ROOT_DIR}")
target_include_directories(JZFramework PUBLIC "${JZFRAMEWORK_ROOT_DIR}")

# ==================== 生成版本头文件 jz_version.h ====================
include("${JZFRAMEWORK_ROOT_DIR}/cmake/jz_version.cmake")

# ==================== 可选第三方依赖 (找得到就接, 找不到就跳过) ====================
# 这里只做**探测**(各自不 REQUIRED, 不影响别的模块); 真正接线由需要它的 target 调
# jz_opencv_enable(<target>) / jz_onnx_enable(<target>) 完成。
#
# 本机第三方库仓库根目录: cmake/opencv.cmake 与 cmake/onnx.cmake 从它下面 glob 找包
# (opencv-*/build[/install], onnxruntime-win-x64-*)。换机器/换盘只改这一处:
#   cmake -S . -B build -DJZ_LIBS_DIR=D:/libs
set(JZ_LIBS_DIR "E:/libs" CACHE PATH "本机第三方库仓库根目录(内含 opencv-*/ 、onnxruntime-win-x64-*/ 等)")
include("${JZFRAMEWORK_ROOT_DIR}/cmake/opencv.cmake")
include("${JZFRAMEWORK_ROOT_DIR}/cmake/onnx.cmake")

# 已添加模块的缓存列表（防止重复添加）
set(JZFRAMEWORK_ADDED_MODULES "" CACHE INTERNAL "")

# 辅助函数：相对路径转绝对路径
function(convert_to_abs_paths VAR_NAME)
    set(ABS_PATHS "")
    foreach(PATH ${${VAR_NAME}})
        if(NOT IS_ABSOLUTE "${PATH}")
            get_filename_component(ABS_PATH "${PATH}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
            list(APPEND ABS_PATHS "${ABS_PATH}")
        else()
            list(APPEND ABS_PATHS "${PATH}")
        endif()
    endforeach()
    set(${VAR_NAME} "${ABS_PATHS}" PARENT_SCOPE)
endfunction()

# ==================== 递归添加模块（内部函数） ====================
function(_jz_declare_model MODULE_NAME)
	# 如果模块已经添加过，则直接返回
    if(${MODULE_NAME} IN_LIST JZFRAMEWORK_ADDED_MODULES)
        return()
    endif()
	
	target_compile_definitions(${JZ_MODULE_TARGET} PUBLIC HAS_${MODULE_NAME})

    # 先递归添加依赖模块
    set(DEPENDS_VAR "MODULE_DEPENDS_${MODULE_NAME}")
    if(DEFINED ${DEPENDS_VAR})
        foreach(DEP ${${DEPENDS_VAR}})
            _jz_declare_model(${DEP})
        endforeach()
    endif()
endfunction()

function(_jz_add_module MODULE_NAME)
    # 如果模块已经添加过，则直接返回
    if(${MODULE_NAME} IN_LIST JZFRAMEWORK_ADDED_MODULES)
        return()
    endif()

    # 先递归添加依赖模块
    set(DEPENDS_VAR "MODULE_DEPENDS_${MODULE_NAME}")
    if(DEFINED ${DEPENDS_VAR})
        foreach(DEP ${${DEPENDS_VAR}})
            _jz_add_module(${DEP})
        endforeach()
    endif()

    # 模块所在目录
    set(MODULE_DIR "${JZFRAMEWORK_ROOT_DIR}/${MODULE_NAME}")
    if(NOT EXISTS "${MODULE_DIR}")
        message(FATAL_ERROR "JZFramework module ${MODULE_NAME} not found at ${MODULE_DIR}")
    endif()

    # 模块的 cmake 配置文件路径
    set(MODULE_CMAKE_FILE "${MODULE_DIR}/${MODULE_NAME}.cmake")
    if(NOT EXISTS "${MODULE_CMAKE_FILE}")
        message(FATAL_ERROR "Module ${MODULE_NAME} is missing its config file: ${MODULE_CMAKE_FILE}")
    endif()

    # 传递全局目标名给模块配置文件
    include("${MODULE_CMAKE_FILE}")

    # 标记模块已添加
    list(APPEND JZFRAMEWORK_ADDED_MODULES ${MODULE_NAME})
    set(JZFRAMEWORK_ADDED_MODULES "${JZFRAMEWORK_ADDED_MODULES}" CACHE INTERNAL "")
endfunction()

# ==================== 模块启用状态(全局可查) ====================
# 背景: 模块的 .cmake 是在 jzframework_use_modules 的**遍历过程中** include 的, 所以
# "是否已添加"(JZFRAMEWORK_ADDED_MODULES)取决于模块在列表里的先后 —— 排在后面的模块
# (如 JZWidget)对排在前面的模块(JZJSEngine)恒为 OFF, 想用就得靠"调用方手动排顺序", 很脆。
#
# 做法: 进入 jzframework_use_modules 时先把"请求的模块 + 传递依赖"**一次性收全**, 存进
# GLOBAL PROPERTY(JZFRAMEWORK_ENABLED_MODULES), 再导出成 HAS_<模块> **缓存变量**。
# 于是任何目录(模块 .cmake / 子目录 CMakeLists)里都能直接写, 与模块顺序无关:
#     if(HAS_JZWidget)
#         list(APPEND MY_SOURCES ...)
#     endif()
# 之所以用缓存变量而不是普通变量: 普通变量只在当前目录作用域可见, 子目录看不到;
# 缓存变量全项目共享 —— 这比"查一次再 if"的封装更好用(那种写法用 PARENT_SCOPE 传结果,
# 在函数里调用时结果只活在那一层)。
# 语义 = "本次构建是否启用该模块", 与 C++ 侧的 HAS_<模块> 编译宏(见 _jz_declare_model)一致。
# 注: 在 jzframework_use_modules **之前**取一律为假 —— 那时还不知道模块列表。

# 递归收集模块及其传递依赖(结果进 GLOBAL PROPERTY, 不受函数作用域影响)
function(_jz_collect_modules MODULE_NAME)
    get_property(_collected GLOBAL PROPERTY JZFRAMEWORK_ENABLED_MODULES)
    if("${MODULE_NAME}" IN_LIST _collected)
        return()   # 已收过: 兼作依赖成环时的终止条件
    endif()
    set_property(GLOBAL APPEND PROPERTY JZFRAMEWORK_ENABLED_MODULES "${MODULE_NAME}")

    set(_depends_var "MODULE_DEPENDS_${MODULE_NAME}")
    if(DEFINED ${_depends_var})
        foreach(_dep ${${_depends_var}})
            _jz_collect_modules("${_dep}")
        endforeach()
    endif()
endfunction()

# 把全局集合导出成 HAS_<模块> 缓存变量: 子目录/模块 .cmake 里可直接 if(HAS_xxx)
function(_jz_export_module_flags)
    # 先清掉上一轮 configure 生成的, 否则"从列表里删掉的模块"会残留 ON
    foreach(_v ${JZFRAMEWORK_HAS_MODULE_VARS})
        unset(${_v} CACHE)
    endforeach()

    get_property(_enabled GLOBAL PROPERTY JZFRAMEWORK_ENABLED_MODULES)
    set(_vars "")
    foreach(_m ${_enabled})
        set(HAS_${_m} ON CACHE INTERNAL "JZFramework: module ${_m} enabled")
        list(APPEND _vars HAS_${_m})
    endforeach()
    set(JZFRAMEWORK_HAS_MODULE_VARS "${_vars}" CACHE INTERNAL "")
endfunction()

# 全局查询: 任何目录/作用域都能问"某模块本次构建是否启用"。
# 查的是 HAS_<模块> 缓存变量, 所以与模块在 jzframework_use_modules 里的先后顺序无关。
function(jz_module_exists MODULE_NAME RESULT_VAR)
    if(HAS_${MODULE_NAME})
        set(${RESULT_VAR} ON PARENT_SCOPE)
    else()
        set(${RESULT_VAR} OFF PARENT_SCOPE)
    endif()
endfunction()

# ==================== 用户接口：设置要启用的模块 ====================
macro(jzframework_use_modules)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs SINGLE_MODULES)
    cmake_parse_arguments(JZ_USE "" "" "${multiValueArgs}" ${ARGN})
	
    # 普通模块 = 所有未被解析的参数
    set(modules ${JZ_USE_UNPARSED_ARGUMENTS})
    # 单例模块 = 显式指定的 SINGLE_MODULES 后的参数
    set(single_modules ${JZ_USE_SINGLE_MODULES})

    # 处理普通模块
    set(JZ_MODULE_TARGET JZFramework)
    if(NOT MSVC)
        target_compile_options(${JZ_MODULE_TARGET} PRIVATE -Wno-psabi)
    endif()
    unset(JZFRAMEWORK_ADDED_MODULES CACHE)

	# 先把"启用集合"算全(见上面"模块启用状态"): 必须在 include 任何模块 .cmake 之前,
	# 这样模块配置里 if(HAS_xxx) 与模块在列表里的顺序无关。
	set_property(GLOBAL PROPERTY JZFRAMEWORK_ENABLED_MODULES "")
	foreach(MODULE ${modules})
		_jz_collect_modules("${MODULE}")
	endforeach()
	foreach(s ${single_modules})
		_jz_collect_modules("${s}")
	endforeach()
	_jz_export_module_flags()

	foreach(MODULE ${modules})
        _jz_declare_model(${MODULE})
    endforeach()
    foreach(MODULE ${modules})
        _jz_add_module(${MODULE})
    endforeach()
    unset(JZ_MODULE_TARGET)

    # 处理单例模块（可选）
    foreach(s ${single_modules})
        add_library(${s} STATIC "")
        set(JZ_MODULE_TARGET ${s})
        include(${JZFRAMEWORK_ROOT_DIR}/${s}/${s}.cmake)
        target_link_libraries(${s} PRIVATE JZFramework)
    endforeach()

    message(STATUS "JZFramework enabled modules: ${JZFRAMEWORK_ADDED_MODULES}")
endmacro()

function(jzframework_use_config TEMPLATE_FILE)
    
    # 模板文件路径：当前调用CMakeLists.txt所在目录
    set(SRC_TEMPLATE "${CMAKE_CURRENT_LIST_DIR}/${TEMPLATE_FILE}")
    # 独立 generated 目录
    set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    # 固定输出文件名
    set(OUTPUT_HEADER "${GEN_DIR}/JZFrameworkConfig.h")

    if(NOT EXISTS "${SRC_TEMPLATE}")
        message(FATAL_ERROR "Template file not found: ${SRC_TEMPLATE}")
    endif()

    file(MAKE_DIRECTORY "${GEN_DIR}")

    configure_file("${SRC_TEMPLATE}" "${OUTPUT_HEADER}" @ONLY)

    # 给 JZFramework 挂载头文件搜索路径
    target_include_directories(JZFramework PUBLIC "${GEN_DIR}")

    # 模板修改自动重新生成
    target_sources(JZFramework PRIVATE "${SRC_TEMPLATE}")

    message(STATUS "[JZFramework] Generate config header: ${OUTPUT_HEADER}")
endfunction()

# ==================== 资源编译: .qrc → .cpp 静态库 ====================
function(jz_add_resources TARGET_NAME)
    set(JZ_RES_PY "${JZFRAMEWORK_ROOT_DIR}/tools/jz_resources.py")
    set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/${TARGET_NAME}")
    file(MAKE_DIRECTORY "${GEN_DIR}")

    set(OUT_CPPS "")

    foreach(QRC_FILE ${ARGN})
        get_filename_component(QRC_NAME "${QRC_FILE}" NAME_WE)
        set(OUT_CPP "${GEN_DIR}/jz_${QRC_NAME}_resources.cpp")
        set(OUT_H "${GEN_DIR}/jz_${QRC_NAME}_resources.h")

        if(NOT IS_ABSOLUTE "${QRC_FILE}")
            set(QRC_ABS "${CMAKE_CURRENT_SOURCE_DIR}/${QRC_FILE}")
        else()
            set(QRC_ABS "${QRC_FILE}")
        endif()

        set(DEPFILE "${OUT_CPP}.d")

        # 解析 .qrc 提取资源文件，显式声明为 DEPENDS（兼容 MSVC 生成器）
        file(READ "${QRC_ABS}" QRC_CONTENT)
        string(REGEX MATCHALL ">[ \t]*([^<>\n]+)[ \t]*<" QRC_MATCHES "${QRC_CONTENT}")
        set(QRC_RESOURCES "")
        get_filename_component(QRC_DIR "${QRC_ABS}" DIRECTORY)
        foreach(MATCH ${QRC_MATCHES})
            # 剥离 > < 和空白
            string(REGEX REPLACE "^>[ \t]*" "" RES "${MATCH}")
            string(REGEX REPLACE "[ \t]*<$" "" RES "${RES}")
            if(EXISTS "${QRC_DIR}/${RES}")
                list(APPEND QRC_RESOURCES "${QRC_DIR}/${RES}")
            endif()
        endforeach()

        add_custom_command(
            OUTPUT "${OUT_CPP}" "${OUT_H}"
            COMMAND ${CMAKE_COMMAND} -E env "${JZ_PYTHON_EXECUTABLE}" "${JZ_RES_PY}" "${QRC_ABS}" -o "${OUT_CPP}" -d "${DEPFILE}"
            DEPENDS "${QRC_ABS}" "${JZ_RES_PY}" ${QRC_RESOURCES}
            DEPFILE "${DEPFILE}"
            COMMENT "Generating resources: ${QRC_FILE} -> ${OUT_CPP}"
        )

        list(APPEND OUT_CPPS "${OUT_CPP}")
        message(STATUS "[JZFramework] Resource: ${QRC_FILE} (${QRC_RESOURCES}) -> ${OUT_CPP}")
    endforeach()

    add_library(${TARGET_NAME} STATIC ${OUT_CPPS})
    # ⚠ 这里**不要**写 target_link_libraries(${TARGET_NAME} PRIVATE JZFramework):
    #   JZFramework 现在默认是动态库, 而资源库又会被它链进去(JZWidget.cmake / JZSimulator.cmake 里的
    #   target_link_libraries(${JZ_MODULE_TARGET} [PUBLIC|PRIVATE] <资源库>)) ⇒ 两个 target 互相依赖 = 环,
    #   CMake 在生成期直接报 "Cyclic dependencies are allowed only among static libraries"
    #   (纯静态库时代能容忍环, 掺进动态库就不行)。
    #   资源代码只用 JZCore 的**头**(JZResources.h), 符号(jzRegisterResource...)由最终链了
    #   JZFramework 的 exe/dll 提供 ⇒ 给包含目录就够。
    target_include_directories(${TARGET_NAME} PRIVATE "${JZFRAMEWORK_ROOT_DIR}/JZCore")
    # 生成的头文件(声明 jzres_xxx_init)加入接口包含目录, 供调用方 #include "jz_<name>_resources.h"
    target_include_directories(${TARGET_NAME} PUBLIC "${GEN_DIR}")
endfunction()

# ==================== Proto -> Json C++ 代码生成 ====================
# 根据 proto 文件生成基于 JZJsonStream 的 C++ 序列化代码 (struct + operator<< / operator>>)
# 用法: jz_add_proto_json(<target> <a.proto> [<b.proto> ...])
#   生成 jz_<name>_pb.h / jz_<name>_pb.cpp 到 build/generated/<target>/ 下, 编译进静态库 <target>
#   调用方按需 #include "jz_<name>_pb.h" 即可使用对应生成的 struct 及其 fromJson / toJson 方法
function(jz_add_proto_json TARGET_NAME)
    set(JZ_PROTO_PY "${JZFRAMEWORK_ROOT_DIR}/tools/jz_proto_json.py")
    set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/${TARGET_NAME}")
    file(MAKE_DIRECTORY "${GEN_DIR}")

    set(OUT_CPPS "")

    foreach(PROTO_FILE ${ARGN})
        get_filename_component(PROTO_NAME "${PROTO_FILE}" NAME_WE)
        set(OUT_CPP "${GEN_DIR}/jz_${PROTO_NAME}_pb.cpp")
        set(OUT_H   "${GEN_DIR}/jz_${PROTO_NAME}_pb.h")
        set(DEPFILE "${OUT_CPP}.d")

        if(NOT IS_ABSOLUTE "${PROTO_FILE}")
            set(PROTO_ABS "${CMAKE_CURRENT_SOURCE_DIR}/${PROTO_FILE}")
        else()
            set(PROTO_ABS "${PROTO_FILE}")
        endif()

        if(NOT EXISTS "${PROTO_ABS}")
            message(FATAL_ERROR "JZFramework: proto file not found: ${PROTO_ABS}")
        endif()

        add_custom_command(
            OUTPUT "${OUT_CPP}" "${OUT_H}"
            COMMAND ${CMAKE_COMMAND} -E env "${JZ_PYTHON_EXECUTABLE}" "${JZ_PROTO_PY}" "${PROTO_ABS}" -o "${OUT_CPP}" -d "${DEPFILE}"
            DEPENDS "${PROTO_ABS}" "${JZ_PROTO_PY}"
            DEPFILE "${DEPFILE}"
            COMMENT "Generating proto json: ${PROTO_FILE} -> ${OUT_CPP}"
        )

        list(APPEND OUT_CPPS "${OUT_CPP}")
        message(STATUS "[JZFramework] Proto: ${PROTO_FILE} -> ${OUT_CPP}")
    endforeach()

    add_library(${TARGET_NAME} STATIC ${OUT_CPPS})
    # ⚠ 同 jz_add_resources: 不能链 JZFramework(本库会被 JZFramework 那侧链进去 ⇒ 环)。
    #   生成代码只用到 JZCore/JZJson 的头, 符号由最终 exe/dll 提供。
    target_include_directories(${TARGET_NAME} PRIVATE "${JZFRAMEWORK_ROOT_DIR}/JZCore")
    # 生成的头文件搜索路径 (供调用方 #include "<name>_pb.h")
    target_include_directories(${TARGET_NAME} PUBLIC "${GEN_DIR}")
    # JZJsonStream / json 头文件路径
    target_include_directories(${TARGET_NAME} PUBLIC "${JZFRAMEWORK_ROOT_DIR}/JZJson")
    target_include_directories(${TARGET_NAME} PUBLIC "${JZFRAMEWORK_ROOT_DIR}/JZJson/json/include")
endfunction()

# ==================== .ui -> C++ 代码生成 (uic 风格) ====================
# 用法: jz_add_ui(<target> <a.ui> [<b.ui> ...])
#   每个 .ui(uiLoader 方言)在构建期生成 <build>/generated/<target>/ui_<name>.h, 内容是
#       namespace Ui { class <name>Ui { ...控件成员... std::map<std::string,JZWidget*> byId;
#                                        void setupUi(); }; }
#   并把该目录加进 <target> 的包含路径 —— 调用方 #include "ui_<name>.h" 后:
#       Ui::<name>Ui ui;  ui.setupUi();  ui.listWidget->appendItem(...);
#
#   与 jz_add_resources 的分工: 那个是"资源 -> 静态库"; 这个是"界面 -> 头文件(编译进 <target>)",
#   所以不另建库、只往 <target> 挂生成头 + 包含目录(风格与 Qt 的 qt5_wrap_ui 类似)。
#   .ui 文件**保留**在源码树里当源头(改它重新构建即重新生成), 生成物在 build 目录里, 不要手改。
#   生成器: tools/jz_uic(C++; 方言逻辑复用 JZUiParser, 与运行期 JZUiLoader / JS 生成器同源)
function(jz_add_ui TARGET_NAME)
    if(NOT TARGET jz_uic)
        message(FATAL_ERROR "JZFramework: jz_add_ui 需要 jz_uic 目标(见 tools/jz_uic); 构建顺序有问题?")
    endif()

    set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/${TARGET_NAME}")
    file(MAKE_DIRECTORY "${GEN_DIR}")

    set(OUT_HS "")
    foreach(UI_FILE ${ARGN})
        # 名字: BTEServer.ui / BTEServer.ui.xml 都取 "BTEServer"
        get_filename_component(UI_NAME "${UI_FILE}" NAME_WE)
        if(UI_NAME MATCHES "\\.ui$")
            string(REGEX REPLACE "\\.ui$" "" UI_NAME "${UI_NAME}")
        endif()

        set(OUT_H "${GEN_DIR}/ui_${UI_NAME}.h")

        if(NOT IS_ABSOLUTE "${UI_FILE}")
            set(UI_ABS "${CMAKE_CURRENT_SOURCE_DIR}/${UI_FILE}")
        else()
            set(UI_ABS "${UI_FILE}")
        endif()
        if(NOT EXISTS "${UI_ABS}")
            message(FATAL_ERROR "JZFramework: .ui 文件不存在: ${UI_ABS}")
        endif()

        add_custom_command(
            OUTPUT "${OUT_H}"
            COMMAND "$<TARGET_FILE:jz_uic>" "${UI_ABS}" -o "${OUT_H}"
            DEPENDS "${UI_ABS}" jz_uic
            COMMENT "Generating UI C++ code: ${UI_FILE} -> ${OUT_H}"
            VERBATIM)

        list(APPEND OUT_HS "${OUT_H}")
        message(STATUS "[JZFramework] UI: ${UI_FILE} -> ${OUT_H}")
    endforeach()

    # 生成头作为 <target> 的源之一: 生成规则随目标走(改 .ui 会重新生成并重新编译)
    target_sources(${TARGET_NAME} PRIVATE ${OUT_HS})
    target_include_directories(${TARGET_NAME} PUBLIC "${GEN_DIR}")
endfunction()