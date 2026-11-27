# JZAudioUtils::audioTrans 测试 (采样格式 / 采样率 / 声道 转换)
# 独立 exe: 不并进 test_core —— 音频转换的用例矩阵比较长, 单独跑更好定位。
add_executable(test_audio main.cpp)
target_link_libraries(test_audio JZFramework)

add_test(NAME audio.test_audio COMMAND test_audio)
set_tests_properties(audio.test_audio PROPERTIES TIMEOUT 120)
