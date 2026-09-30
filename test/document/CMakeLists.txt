add_executable(test_document test_document.cpp)
target_link_libraries(test_document JZFramework)

add_test(NAME document.test_document COMMAND test_document)
set_tests_properties(document.test_document PROPERTIES TIMEOUT 120)
