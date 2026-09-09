add_executable(test_jsengine test_jsengine.cpp)
target_link_libraries(test_jsengine JZFramework)

add_executable(test_jsui test_jsui.cpp)
target_link_libraries(test_jsui JZFramework)

add_executable(test_jsnet test_jsnet.cpp)
target_link_libraries(test_jsnet JZFramework)

# ---- ctest 注册 ----
add_test(NAME jsengine.test_jsengine COMMAND test_jsengine)
set_tests_properties(jsengine.test_jsengine PROPERTIES TIMEOUT 300)

add_test(NAME jsengine.test_jsui COMMAND test_jsui)
set_tests_properties(jsengine.test_jsui PROPERTIES TIMEOUT 60)

add_test(NAME jsengine.test_jsnet COMMAND test_jsnet)
set_tests_properties(jsengine.test_jsnet PROPERTIES TIMEOUT 300)
