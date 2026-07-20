add_executable(test_co 
	test_co.cpp
	test_coBasic.cpp
	test_coBle.cpp
	test_coHttp.cpp
	test_coSerialPort.cpp
	test_coTcp.cpp
	test_coUdp.cpp
	test_coWebSocket.cpp)
target_link_libraries(test_co JZFramework)