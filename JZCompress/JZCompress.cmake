set(JZCOMPRESS_SOURCES
    JZCompress.h
    JZCompress.cpp
)

# ---- miniz 后端（public domain / unlicense）----
# 提供 deflate/zlib/gzip 流式压缩解压 + zip 归档（mz_zip）。
# 当前已用：miniz.c(tdefl/tinfl 壳) + miniz_tdef.c(压缩内核) + miniz_tinfl.c(解压内核)。
# miniz_zip.c 已编入以备后续 JZZip/JZUnZip 接入；MCU 上若不需要 zip 归档，
# 可移除 miniz_zip.c 并定义 MINIZ_NO_ARCHIVE_WRITING_APIS 等裁剪宏以减小体积。
set(JZCOMPRESS_MINIZ_SOURCES
    miniz/miniz.c
    miniz/miniz_tdef.c
    miniz/miniz_tinfl.c
    miniz/miniz_zip.c
)

convert_to_abs_paths(JZCOMPRESS_SOURCES)
convert_to_abs_paths(JZCOMPRESS_MINIZ_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZCOMPRESS_SOURCES} ${JZCOMPRESS_MINIZ_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
