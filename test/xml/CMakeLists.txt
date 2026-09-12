add_executable(test_xml test_xml.cpp)
target_link_libraries(test_xml JZFramework)

add_test(NAME xml.test_xml COMMAND test_xml)
set_tests_properties(xml.test_xml PROPERTIES TIMEOUT 120)
