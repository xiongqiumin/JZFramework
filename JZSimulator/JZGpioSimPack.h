#ifndef JZ_GPIO_SIM_PACK_H_
#define JZ_GPIO_SIM_PACK_H_

#include "JZNetPacket.h"
#include "JZVariant.h"
// 复用 JZBleSimPack.h 中的 simGetInt/simSetInt/simGetStr/simSetStr 辅助函数
#include "JZBleSimPack.h"

// 模拟 GPIO 通信消息类型 (独立的网络包类型, 与 BLE 互不影响)
enum JZGpioSimMsg {
    SIM_GPIO_NONE = 0,

    // Driver -> Device (命令)
    SIM_GPIO_CMD_OPEN,        // pin, dir, pull
    SIM_GPIO_CMD_CLOSE,       // pin
    SIM_GPIO_CMD_SET_DIR,     // pin, dir
    SIM_GPIO_CMD_SET_PULL,    // pin, pull
    SIM_GPIO_CMD_WRITE,       // pin, level
    SIM_GPIO_CMD_READ,        // pin
    SIM_GPIO_CMD_PWM_START,   // pin, freq, duty(千分比)
    SIM_GPIO_CMD_PWM_FREQ,    // pin, freq
    SIM_GPIO_CMD_PWM_DUTY,    // pin, duty(千分比)
    SIM_GPIO_CMD_PWM_STOP,    // pin
    SIM_GPIO_CMD_WAVE_START,  // pin, wave, freq, amp, offset
    SIM_GPIO_CMD_WAVE_STOP,   // pin
    SIM_GPIO_CMD_SET_TRIGGER, // pin, edge

    // Device -> Driver (事件)
    SIM_GPIO_EVT_LEVEL,       // pin, level  引脚电平变化
    SIM_GPIO_EVT_READ_RESULT, // pin, level  读结果
};

// 模拟 GPIO 通信用网络包。整包 payload 为一个 JZVariantMap,
// 其中 "msg" 字段表示消息类型, 其余字段为各消息的参数。
class JZGpioSimPack : public JZNetPack
{
public:
    JZGpioSimPack();
    int packetType() const override;
    void saveToStream(JZDataStream &s) const override;
    void loadFromStream(JZDataStream &s) override;

    // 确保包类型已注册 (幂等)
    static void ensureRegistered();

    JZVariantMap map;

    int msg() const { return simGetInt(map, "msg"); }
};

#endif // !JZ_GPIO_SIM_PACK_H_
