#ifndef JZ_SERIAL_PORT_INFO_H_
#define JZ_SERIAL_PORT_INFO_H_

#include <string>
#include <vector>

// 描述一个可用串口的静态信息（设备枚举结果）。
// 各平台驱动在 JZSerialPortDriver::availablePorts() 中填充并上报。
class JZSerialPortInfo
{
public:
    std::string id;                // 设备唯一标识（如系统分配的实例 ID）
    std::string portName;          // 端口名，如 "COM3" / "/dev/ttyUSB0" / "UART1"
    std::string description;       // 描述，如 "USB Serial"

    // 是否为空（无有效设备 id）
    bool isNull() const { return id.empty(); }
};

#endif // !JZ_SERIAL_PORT_INFO_H_
