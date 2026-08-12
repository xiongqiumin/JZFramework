#ifndef JZ_GPIO_INFO_H_
#define JZ_GPIO_INFO_H_

#include <string>
#include <vector>

// 描述一个可用 GPIO 引脚的静态信息（引脚枚举结果）。
// 各平台驱动在 JZGpioDriver::availableGpios() 中填充并上报。
class JZGpioInfo
{
public:
    // 引脚能力(位掩码, 可组合)
    enum Capability {
        CapNone   = 0,
        CapInput  = 0x1,   // 可作输入
        CapOutput = 0x2,   // 可作输出
        CapInputOutput = CapInput | CapOutput,  // 输入/输出均可
    };

    
    // 是否为空（无有效引脚号）
    bool isNull() const { return pin < 0; }

    // 能力查询/设置
    void setCapability(Capability cap) { capability = cap; }
    bool isInput()  const { return (capability & CapInput)  != 0; }
    bool isOutput() const { return (capability & CapOutput) != 0; }

    int         pin = -1;       // 引脚号
    std::string name;           // 引脚名, 如 "GPIO2"
    std::string description;    // 描述, 如 "通用 IO"

    Capability  capability = CapInputOutput;  // 引脚能力
};

#endif // !JZ_GPIO_INFO_H_
