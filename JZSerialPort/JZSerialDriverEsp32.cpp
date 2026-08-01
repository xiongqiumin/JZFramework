#include "JZSerialDriverEsp32.h"
#include "JZSerialPort.h"
#include "JZStd.h"

#include "esp_err.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace
{
struct Esp32TaskArg
{
    JZSerialDriverEsp32 *self;
    JZSerialPort        *port;
    std::string          portName;
};

inline uart_word_length_t toWordLength(JZSerialPort::DataBits db)
{
    switch (db)
    {
    case JZSerialPort::Data5: return UART_DATA_5_BITS;
    case JZSerialPort::Data6: return UART_DATA_6_BITS;
    case JZSerialPort::Data7: return UART_DATA_7_BITS;
    case JZSerialPort::Data8: return UART_DATA_8_BITS;
    }
    return UART_DATA_8_BITS;
}

inline uart_stop_bits_t toStopBits(JZSerialPort::StopBits sb)
{
    switch (sb)
    {
    case JZSerialPort::OneStop:       return UART_STOP_BITS_1;
    case JZSerialPort::OneAndHalfStop: return UART_STOP_BITS_1_5;
    case JZSerialPort::TwoStop:       return UART_STOP_BITS_2;
    }
    return UART_STOP_BITS_1;
}

inline uart_parity_t toParity(JZSerialPort::Parity p)
{
    switch (p)
    {
    case JZSerialPort::EvenParity: return UART_PARITY_EVEN;
    case JZSerialPort::OddParity:  return UART_PARITY_ODD;
    case JZSerialPort::NoParity:
    default:                        return UART_PARITY_DISABLE;
    }
    // Space/Mark 校验在 ESP-IDF 无直接枚举，暂按无校验处理
}
} // namespace

// ==================== 构造 / 析构 ====================
JZSerialDriverEsp32::JZSerialDriverEsp32() = default;

JZSerialDriverEsp32::~JZSerialDriverEsp32()
{
    // 关闭所有仍处于打开状态的端口
    std::vector<std::string> names;
    for (auto &kv : m_ports)
        names.push_back(kv.first);
    for (auto &n : names)
    {
        // 通过临时查找端口对象不可行，这里仅做驱动卸载清理
        auto it = m_ports.find(n);
        if (it != m_ports.end() && it->second.uartNum >= 0)
            uart_driver_delete((uart_port_t)it->second.uartNum);
    }
    m_ports.clear();
}

void JZSerialDriverEsp32::setDefaultPins(int tx, int rx, int rts, int cts)
{
    m_defTx = tx;
    m_defRx = rx;
    m_defRts = rts;
    m_defCts = cts;
}

// ==================== 工具 ====================
int JZSerialDriverEsp32::parseUartNum(const std::string &portName)
{
    size_t i = portName.find_last_not_of("0123456789");
    std::string digits = (i == std::string::npos) ? portName : portName.substr(i + 1);
    if (digits.empty())
        return -1;
    int v = str_toInt(digits);
    return (v >= 0 && v <= 2) ? v : -1;   // ESP32 通常提供 UART0~UART2
}

// ==================== 接收任务 ====================
void JZSerialDriverEsp32::readTask(void *arg)
{
    auto *ta = static_cast<Esp32TaskArg *>(arg);
    auto it = ta->self->m_ports.find(ta->portName);
    if (it == ta->self->m_ports.end())
    {
        delete ta;
        vTaskDelete(nullptr);
        return;
    }

    const int uart = it->second.uartNum;
    const int bufSize = 256;
    std::vector<char> buf(bufSize);

    while (it->second.running)
    {
        int len = uart_read_bytes((uart_port_t)uart, buf.data(), bufSize, pdMS_TO_TICKS(20));
        if (len > 0)
        {
            JZByteArray data(buf.data(), len);
            ta->port->onDataReceived(data);
        }
    }

    delete ta;
    vTaskDelete(nullptr);
}

// ==================== 打开 / 关闭 ====================
bool JZSerialDriverEsp32::open(JZSerialPort *port)
{
    std::string name = port->portName();
    int uart = parseUartNum(name);
    if (uart < 0)
        return false;

    PortState &st = m_ports[name];
    st.uartNum = uart;
    st.txPin = m_defTx;
    st.rxPin = m_defRx;
    st.rtsPin = m_defRts;
    st.ctsPin = m_defCts;

    uart_config_t cfg = {};
    cfg.baud_rate = (uint32_t)port->baudRate();
    cfg.data_bits = toWordLength(port->dataBits());
    cfg.parity = toParity(port->parity());
    cfg.stop_bits = toStopBits(port->stopBits());
    cfg.flow_ctrl = (port->flowControl() == JZSerialPort::HardwareControl)
                        ? UART_HW_FLOWCTRL_CTS_RTS
                        : UART_HW_FLOWCTRL_DISABLE;
    cfg.rx_flow_ctrl_thresh = 122;
    cfg.source_clk = UART_SCLK_DEFAULT;

    if (uart_param_config((uart_port_t)uart, &cfg) != ESP_OK)
        return false;
    if (uart_set_pin((uart_port_t)uart, st.txPin, st.rxPin, st.rtsPin, st.ctsPin) != ESP_OK)
        return false;
    if (uart_driver_install((uart_port_t)uart, 2048, 2048, 0, nullptr, 0) != ESP_OK)
        return false;

    if (port->flowControl() == JZSerialPort::HardwareControl)
        uart_set_hw_flow_ctrl((uart_port_t)uart, UART_HW_FLOWCTRL_CTS_RTS, 122);

    st.running = true;
    auto *ta = new Esp32TaskArg{this, port, name};
    st.taskHandle = nullptr;
    if (xTaskCreate(readTask, "jz_serial_rx", 4096, ta, 10,
                    (TaskHandle_t *)&st.taskHandle) != pdPASS)
    {
        delete ta;
        st.running = false;
        uart_driver_delete((uart_port_t)uart);
        m_ports.erase(name);
        return false;
    }
    return true;
}

void JZSerialDriverEsp32::close(JZSerialPort *port)
{
    std::string name = port->portName();
    auto it = m_ports.find(name);
    if (it == m_ports.end())
        return;

    it->second.running = false;
    // 等待接收任务自行退出
    vTaskDelay(pdMS_TO_TICKS(40));

    if (it->second.uartNum >= 0)
        uart_driver_delete((uart_port_t)it->second.uartNum);

    m_ports.erase(it);
}

bool JZSerialDriverEsp32::isOpen(JZSerialPort *port)
{
    auto it = m_ports.find(port->portName());
    return it != m_ports.end() && it->second.running;
}

// ==================== 枚举可用串口 ====================
std::vector<JZSerialPortInfo> JZSerialDriverEsp32::availablePorts()
{
    // ESP32 的串口为固定的硬件 UART，无动态热插拔枚举，这里返回芯片提供的端口列表。
    std::vector<JZSerialPortInfo> list;
    const char *names[] = {"UART0", "UART1", "UART2"};
    for (int i = 0; i < 3; i++)
    {
        JZSerialPortInfo info;
        info.id = names[i];
        info.portName = names[i];
        info.description = "ESP32 Hardware UART";
        list.push_back(info);
    }
    return list;
}

// ==================== 收发 ====================
int JZSerialDriverEsp32::read(JZSerialPort *port, char *buf, int maxSize)
{
    auto it = m_ports.find(port->portName());
    if (it == m_ports.end() || maxSize <= 0)
        return -1;
    int len = uart_read_bytes((uart_port_t)it->second.uartNum, buf, maxSize, 0);
    return len;   // 0: 暂无数据, -1: 错误
}

int JZSerialDriverEsp32::write(JZSerialPort *port, const char *buf, int len)
{
    auto it = m_ports.find(port->portName());
    if (it == m_ports.end() || len <= 0)
        return 0;
    int n = uart_write_bytes((uart_port_t)it->second.uartNum, buf, (size_t)len);
    return n;
}

// ==================== 参数设置 ====================
bool JZSerialDriverEsp32::setBaudRate(JZSerialPort *port, int32_t baudRate)
{
    auto it = m_ports.find(port->portName());
    if (it == m_ports.end())
        return false;
    return uart_set_baudrate((uart_port_t)it->second.uartNum, (uint32_t)baudRate) == ESP_OK;
}

bool JZSerialDriverEsp32::setDataBits(JZSerialPort *port, JZSerialPort::DataBits dataBits)
{
    auto it = m_ports.find(port->portName());
    if (it == m_ports.end())
        return false;
    return uart_set_word_length((uart_port_t)it->second.uartNum, toWordLength(dataBits)) == ESP_OK;
}

bool JZSerialDriverEsp32::setParity(JZSerialPort *port, JZSerialPort::Parity parity)
{
    auto it = m_ports.find(port->portName());
    if (it == m_ports.end())
        return false;
    return uart_set_parity((uart_port_t)it->second.uartNum, toParity(parity)) == ESP_OK;
}

bool JZSerialDriverEsp32::setStopBits(JZSerialPort *port, JZSerialPort::StopBits stopBits)
{
    auto it = m_ports.find(port->portName());
    if (it == m_ports.end())
        return false;
    return uart_set_stop_bits((uart_port_t)it->second.uartNum, toStopBits(stopBits)) == ESP_OK;
}

bool JZSerialDriverEsp32::setFlowControl(JZSerialPort *port, JZSerialPort::FlowControl flowControl)
{
    auto it = m_ports.find(port->portName());
    if (it == m_ports.end())
        return false;
    if (flowControl == JZSerialPort::HardwareControl)
        return uart_set_hw_flow_ctrl((uart_port_t)it->second.uartNum,
                                     UART_HW_FLOWCTRL_CTS_RTS, 122) == ESP_OK;
    return uart_set_hw_flow_ctrl((uart_port_t)it->second.uartNum,
                                 UART_HW_FLOWCTRL_DISABLE, 0) == ESP_OK;
}

bool JZSerialDriverEsp32::setDtr(JZSerialPort *port, bool level)
{
    auto it = m_ports.find(port->portName());
    if (it == m_ports.end())
        return false;
    return uart_set_dtr((uart_port_t)it->second.uartNum, level ? 1 : 0) == ESP_OK;
}

bool JZSerialDriverEsp32::setRts(JZSerialPort *port, bool level)
{
    auto it = m_ports.find(port->portName());
    if (it == m_ports.end())
        return false;
    return uart_set_rts((uart_port_t)it->second.uartNum, level ? 1 : 0) == ESP_OK;
}
