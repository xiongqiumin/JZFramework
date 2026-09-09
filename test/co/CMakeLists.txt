add_executable(test_co 
	test_co.cpp
	test_coBasic.cpp	
	test_coHttp.cpp
	test_coSerialPort.cpp
	test_coTcp.cpp
	test_coUdp.cpp
	test_coWebSocket.cpp)

jz_module_exists(JZBle HAS_JZBLE)
if(HAS_JZBLE)
    list(APPEND JZCO_SOURCES test_coBle.cpp)
endif()

target_link_libraries(test_co JZFramework)

# ---- ctest 注册 (仅自包含用例; BLE/串口等无硬件时跳过) ----
add_test(NAME co.test_co COMMAND test_co)
set_tests_properties(co.test_co PROPERTIES TIMEOUT 180)