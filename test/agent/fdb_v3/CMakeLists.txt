# FDB-v3 文本模式跑批（C++ / JZAgent）。
# ⚠ 不进 ctest：它要连真实模型端点（局域网/公网），且一条跑完要几十秒~几分钟。
add_executable(fdb_v3_text fdb_v3_text.cpp)
target_link_libraries(fdb_v3_text JZFramework)
