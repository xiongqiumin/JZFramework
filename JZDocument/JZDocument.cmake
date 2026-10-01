set(JZDOCUMENT_SOURCES
    JZDocument.h
    JZDocument.cpp
    JZDocumentInternal.h
    JZDocumentInternal.cpp
    JZDocxReader.cpp
    JZXlsxReader.cpp
    JZPdfBackend.cpp
)

convert_to_abs_paths(JZDOCUMENT_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZDOCUMENT_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
