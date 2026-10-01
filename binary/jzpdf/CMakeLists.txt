# ---------------------------------------------------------------------------
# jzpdf —— JZDocument 的 PDF 后端 CLI(PDFium 内核, 独立进程)
#
# 不默认构建: 需要 PDFium 预编译包([bblanchon/pdfium-binaries] 的 tgz 解压目录,
# 里面有 include/ 与 lib/)。配置时指定:
#   cmake -S . -B build -DJZ_PDFIUM_DIR=E:/libs/pdfium/win-x64
# 没给(或路径无效)就跳过 —— JZFramework 与 JZDocument 照常编译, 只是
# JZDocument::open("x.pdf") 会返回 "找不到后端" 的错误。
# ---------------------------------------------------------------------------
if(NOT JZ_PDFIUM_DIR)
    message(STATUS "[jzpdf] JZ_PDFIUM_DIR not set -> skip (PDF backend unavailable)")
    return()
endif()
if(NOT EXISTS "${JZ_PDFIUM_DIR}")
    message(STATUS "[jzpdf] JZ_PDFIUM_DIR=${JZ_PDFIUM_DIR} not found -> skip")
    return()
endif()

if(WIN32)
    set(JZ_PDFIUM_LIB "${JZ_PDFIUM_DIR}/lib/pdfium.dll.lib")
    set(JZ_PDFIUM_SHARED "${JZ_PDFIUM_DIR}/bin/pdfium.dll")
else()
    set(JZ_PDFIUM_LIB "${JZ_PDFIUM_DIR}/lib/libpdfium.so")
    set(JZ_PDFIUM_SHARED "${JZ_PDFIUM_LIB}")
endif()

if(NOT EXISTS "${JZ_PDFIUM_LIB}")
    message(WARNING "[jzpdf] pdfium library not found at ${JZ_PDFIUM_LIB} -> skip")
    return()
endif()

add_executable(jzpdf main.cpp)
target_include_directories(jzpdf PRIVATE "${JZ_PDFIUM_DIR}/include")
target_link_libraries(jzpdf PRIVATE JZFramework "${JZ_PDFIUM_LIB}")

# Windows: pdfium.dll 必须和 exe 同目录, 否则起不来(0xC0000135)
if(WIN32 AND EXISTS "${JZ_PDFIUM_SHARED}")
    add_custom_command(TARGET jzpdf POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${JZ_PDFIUM_SHARED}" "$<TARGET_FILE_DIR:jzpdf>"
        COMMENT "copy pdfium.dll next to jzpdf")
endif()

message(STATUS "[jzpdf] PDF backend enabled (pdfium: ${JZ_PDFIUM_LIB})")
