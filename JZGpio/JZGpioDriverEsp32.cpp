#include "JZGpioDriverEsp32.h"

#include <math.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/dac.h"
#include "soc/gpio_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

namespace
{
constexpr int    LEDC_TIMER_BITS = 13;         // 8192 级占空比分辨率
constexpr int    LEDC_MAX_DUTY   = (1 << LEDC_TIMER_BITS) - 1;
constexpr int    WAVE_UPDATE_HZ  = 200;        // 波形软件采样率(独立任务周期)

// ISR -> 任务 传递的事件
struct IsrEvent
{
    int pin;
    int level;
};

inline bool isDacPin(int pin) { return pin == 25 || pin == 26; }
} // namespace

JZGpioDriverEsp32::JZGpioDriverEsp32() = default;

// ==================== 引脚枚举 ====================
// ESP-IDF 没有运行时枚举 GPIO 的接口, 但提供编译期芯片能力宏:
//   SOC_GPIO_PIN_COUNT              引脚数量
//   SOC_GPIO_VALID_GPIO_MASK        物理有效 GPIO 位掩码
//   SOC_GPIO_VALID_OUTPUT_GPIO_MASK 可输出脚掩码(用于区分 34~39 输入专用脚)
// 注意: 这些宏只能反映"物理存在/能否输出", 无法反映板级占用
// (Flash/PSRAM/UART/Strapping 占用情况需按模组裁剪)。
std::vector<JZGpioInfo> JZGpioDriverEsp32::availableGpios()
{
    std::vector<JZGpioInfo> list;
    for (int p = 0; p < SOC_GPIO_PIN_COUNT; p++)
    {
        if (!((SOC_GPIO_VALID_GPIO_MASK >> p) & 0x1))
            continue;   // 该脚在当前芯片上不存在

        JZGpioInfo info;
        info.pin = p;
        info.name = "GPIO" + std::to_string(p);
        // 输入专用脚(如 ESP32 的 34~39)做标注
        bool outputCapable = (SOC_GPIO_VALID_OUTPUT_GPIO_MASK >> p) & 0x1;
        info.description = outputCapable ? "ESP32 GPIO" : "ESP32 GPIO (input only)";
        list.push_back(info);
    }
    return list;
}

JZGpioDriverEsp32::~JZGpioDriverEsp32()
{
    // 停止波形任务 (m_running=false 后任务自行退出)
    m_running = false;
    if (m_waveTask)
    {
        vTaskDelay(pdMS_TO_TICKS(30));
        m_waveTask = nullptr;
    }
    // 停止分发任务
    if (m_dispatchTask)
    {
        vTaskDelay(pdMS_TO_TICKS(30));
        m_dispatchTask = nullptr;
    }

    std::vector<int> pins;
    for (auto &kv : m_pins)
        pins.push_back(kv.first);
    for (int p : pins)
    {
        auto it = m_pins.find(p);
        if (it != m_pins.end())
        {
            if (it->second.pwmRunning && it->second.ledcChannel >= 0)
                freeLedcChannel(it->second.ledcChannel);
            if (it->second.isrAdded)
                gpio_isr_handler_remove((gpio_num_t)p);
        }
    }
    m_pins.clear();

    if (m_isrServiceInstalled)
        gpio_uninstall_isr_service();
    if (m_isrQueue)
        vQueueDelete((QueueHandle_t)m_isrQueue);
    if (m_waveMutex)
    {
        vSemaphoreDelete((SemaphoreHandle_t)m_waveMutex);
        m_waveMutex = nullptr;
    }
}

// ==================== LEDC 通道分配 ====================
int JZGpioDriverEsp32::allocLedcChannel()
{
    for (int ch = 0; ch < LEDC_CHANNEL_MAX; ch++)
    {
        if (!(m_ledcChannelMask & (1u << ch)))
        {
            m_ledcChannelMask |= (1u << ch);
            return ch;
        }
    }
    return -1;
}

void JZGpioDriverEsp32::freeLedcChannel(int ch)
{
    if (ch < 0)
        return;
    ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch, 0);
    m_ledcChannelMask &= ~(1u << ch);
}

// ==================== ISR / 分发 ====================
void IRAM_ATTR JZGpioDriverEsp32::isrHandler(void *arg)
{
    // arg 为 driver 实例; 在 ISR 中不能调用复杂逻辑, 仅把引脚号投递到队列。
    auto *self = static_cast<JZGpioDriverEsp32 *>(arg);
    if (!self || !self->m_isrQueue)
        return;
    // 无法在 ISR 内枚举触发引脚, 交由任务侧统一读电平比对。这里投递一个唤醒事件。
    IsrEvent ev{-1, -1};
    BaseType_t hpTaskWoken = pdFALSE;
    xQueueSendFromISR((QueueHandle_t)self->m_isrQueue, &ev, &hpTaskWoken);
    if (hpTaskWoken)
        portYIELD_FROM_ISR();
}

void JZGpioDriverEsp32::dispatchTask(void *arg)
{
    auto *self = static_cast<JZGpioDriverEsp32 *>(arg);
    IsrEvent ev;
    // 记录每个引脚上一次电平, 用于在任务上下文中判定变化
    while (self->m_running)
    {
        if (xQueueReceive((QueueHandle_t)self->m_isrQueue, &ev, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            // 逐个输入引脚复查电平, 变化则回调
            for (auto &kv : self->m_pins)
            {
                PinState &st = kv.second;
                if (st.dir != JZGpio::Input || st.edge == JZGpio::NoEdge || !st.gpio)
                    continue;
                int lv = gpio_get_level((gpio_num_t)kv.first);
                st.gpio->onLevelChanged(lv);
            }
        }
    }
    vTaskDelete(nullptr);
}

// ==================== 打开 / 关闭 ====================
bool JZGpioDriverEsp32::open(JZGpio *gpio)
{
    int pin = gpio->pin();
    if (pin < 0)
        return false;

    PinState &st = m_pins[pin];
    st.gpio = gpio;
    st.dir = gpio->direction();
    st.pull = gpio->pull();
    st.edge = gpio->triggerEdge();

    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << pin);
    cfg.mode = (st.dir == JZGpio::Output) ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT;
    cfg.pull_up_en = (st.pull == JZGpio::PullUp) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = (st.pull == JZGpio::PullDown) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;

    if (gpio_config(&cfg) != ESP_OK)
    {
        m_pins.erase(pin);
        return false;
    }

    // 惰性初始化 ISR 服务 + 分发任务
    if (!m_isrQueue)
        m_isrQueue = xQueueCreate(16, sizeof(IsrEvent));
    if (!m_isrServiceInstalled)
        m_isrServiceInstalled = (gpio_install_isr_service(0) == ESP_OK);
    if (!m_running)
    {
        m_running = true;
        xTaskCreate(dispatchTask, "jz_gpio_disp", 4096, this, 10,
                    (TaskHandle_t *)&m_dispatchTask);
    }

    // 若在 open 前已配置触发边沿, 立即应用
    if (st.dir == JZGpio::Input && st.edge != JZGpio::NoEdge)
        setTriggerEdge(gpio, st.edge);

    return true;
}

void JZGpioDriverEsp32::close(JZGpio *gpio)
{
    int pin = gpio->pin();
    auto it = m_pins.find(pin);
    if (it == m_pins.end())
        return;

    if (m_waveMutex)
        xSemaphoreTake((SemaphoreHandle_t)m_waveMutex, portMAX_DELAY);
    PinState &st = it->second;
    st.waveRunning = false;            // 波形任务会在下一轮检测到并停止输出
    if (st.pwmRunning && st.ledcChannel >= 0)
        freeLedcChannel(st.ledcChannel);
    if (st.isrAdded)
        gpio_isr_handler_remove((gpio_num_t)pin);

    gpio_reset_pin((gpio_num_t)pin);
    m_pins.erase(it);
    if (m_waveMutex)
        xSemaphoreGive((SemaphoreHandle_t)m_waveMutex);
}

bool JZGpioDriverEsp32::isOpen(JZGpio *gpio)
{
    return m_pins.find(gpio->pin()) != m_pins.end();
}

// ==================== 配置 ====================
bool JZGpioDriverEsp32::setDirection(JZGpio *gpio, JZGpio::Direction dir)
{
    auto it = m_pins.find(gpio->pin());
    if (it == m_pins.end())
        return false;
    it->second.dir = dir;
    gpio_mode_t m = (dir == JZGpio::Output) ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT;
    return gpio_set_direction((gpio_num_t)gpio->pin(), m) == ESP_OK;
}

bool JZGpioDriverEsp32::setPull(JZGpio *gpio, JZGpio::Pull pull)
{
    auto it = m_pins.find(gpio->pin());
    if (it == m_pins.end())
        return false;
    it->second.pull = pull;
    gpio_pull_mode_t m = GPIO_FLOATING;
    if (pull == JZGpio::PullUp)   m = GPIO_PULLUP_ONLY;
    if (pull == JZGpio::PullDown) m = GPIO_PULLDOWN_ONLY;
    return gpio_set_pull_mode((gpio_num_t)gpio->pin(), m) == ESP_OK;
}

// ==================== 数字 IO ====================
bool JZGpioDriverEsp32::write(JZGpio *gpio, int level)
{
    auto it = m_pins.find(gpio->pin());
    if (it == m_pins.end())
        return false;
    return gpio_set_level((gpio_num_t)gpio->pin(), level ? 1 : 0) == ESP_OK;
}

int JZGpioDriverEsp32::read(JZGpio *gpio)
{
    if (m_pins.find(gpio->pin()) == m_pins.end())
        return -1;
    return gpio_get_level((gpio_num_t)gpio->pin());
}

// ==================== PWM (LEDC) ====================
void JZGpioDriverEsp32::applyPwm(PinState &st)
{
    int duty = (int)(st.pwmDuty * LEDC_MAX_DUTY + 0.5f);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)st.ledcChannel, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)st.ledcChannel);
}

bool JZGpioDriverEsp32::startPwm(JZGpio *gpio, int freqHz, float duty)
{
    auto it = m_pins.find(gpio->pin());
    if (it == m_pins.end())
        return false;
    PinState &st = it->second;

    if (st.ledcChannel < 0)
    {
        st.ledcChannel = allocLedcChannel();
        if (st.ledcChannel < 0)
            return false;
    }
    st.pwmFreq = freqHz;
    st.pwmDuty = duty;

    ledc_timer_config_t tcfg = {};
    tcfg.speed_mode = LEDC_LOW_SPEED_MODE;
    tcfg.duty_resolution = (ledc_timer_bit_t)LEDC_TIMER_BITS;
    tcfg.timer_num = LEDC_TIMER_0;
    tcfg.freq_hz = (uint32_t)freqHz;
    tcfg.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&tcfg) != ESP_OK)
        return false;

    ledc_channel_config_t ccfg = {};
    ccfg.gpio_num = gpio->pin();
    ccfg.speed_mode = LEDC_LOW_SPEED_MODE;
    ccfg.channel = (ledc_channel_t)st.ledcChannel;
    ccfg.timer_sel = LEDC_TIMER_0;
    ccfg.duty = (int)(duty * LEDC_MAX_DUTY + 0.5f);
    ccfg.hpoint = 0;
    if (ledc_channel_config(&ccfg) != ESP_OK)
        return false;

    st.pwmRunning = true;
    return true;
}

bool JZGpioDriverEsp32::setPwmFrequency(JZGpio *gpio, int freqHz)
{
    auto it = m_pins.find(gpio->pin());
    if (it == m_pins.end() || !it->second.pwmRunning)
        return false;
    it->second.pwmFreq = freqHz;
    return ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, (uint32_t)freqHz) == ESP_OK;
}

bool JZGpioDriverEsp32::setPwmDuty(JZGpio *gpio, float duty)
{
    auto it = m_pins.find(gpio->pin());
    if (it == m_pins.end() || !it->second.pwmRunning)
        return false;
    it->second.pwmDuty = duty;
    applyPwm(it->second);
    return true;
}

void JZGpioDriverEsp32::stopPwm(JZGpio *gpio)
{
    auto it = m_pins.find(gpio->pin());
    if (it == m_pins.end() || !it->second.pwmRunning)
        return;
    freeLedcChannel(it->second.ledcChannel);
    it->second.ledcChannel = -1;
    it->second.pwmRunning = false;
}

// ==================== 模拟波形 (独立任务) ====================
void JZGpioDriverEsp32::ensureWaveTask()
{
    if (m_waveTask)
        return;
    if (!m_waveMutex)
        m_waveMutex = xSemaphoreCreateMutex();
    xTaskCreate(waveTask, "jz_gpio_wave", 3072, this, 9, (TaskHandle_t *)&m_waveTask);
}

void JZGpioDriverEsp32::waveTask(void *arg)
{
    auto *self = static_cast<JZGpioDriverEsp32 *>(arg);
    const TickType_t period = pdMS_TO_TICKS(1000 / WAVE_UPDATE_HZ);

    while (self->m_running)
    {
        bool anyActive = false;

        if (self->m_waveMutex)
            xSemaphoreTake((SemaphoreHandle_t)self->m_waveMutex, portMAX_DELAY);

        for (auto &kv : self->m_pins)
        {
            PinState &st = kv.second;
            if (!st.waveRunning)
                continue;
            anyActive = true;

            // 相位推进
            double dt = 1.0 / WAVE_UPDATE_HZ;
            st.wavePhase += st.waveFreq * dt;
            while (st.wavePhase >= 1.0)
                st.wavePhase -= 1.0;
            double ph = st.wavePhase;  // 0~1

            // 归一化采样 0~1
            double s = 0.0;
            switch (st.waveform)
            {
            case JZGpio::WaveSquare:   s = (ph < 0.5) ? 1.0 : 0.0; break;
            case JZGpio::WaveSine:     s = 0.5 * (1.0 + sin(2.0 * M_PI * ph)); break;
            case JZGpio::WaveTriangle: s = (ph < 0.5) ? (ph * 2.0) : (2.0 - ph * 2.0); break;
            case JZGpio::WaveSawtooth: s = ph; break;
            case JZGpio::WavePulse:    s = (ph < 0.05) ? 1.0 : 0.0; break;
            default:                   s = 0.0; break;
            }

            int value = st.waveOffset + (int)(s * st.waveAmp + 0.5);

            if (st.useDac && st.gpio)
            {
                int v = value;
                if (v < 0) v = 0;
                if (v > 255) v = 255;
                dac_channel_t ch = (st.gpio->pin() == 25) ? DAC_CHANNEL_1 : DAC_CHANNEL_2;
                dac_output_voltage(ch, (uint8_t)v);
            }
            else if (st.pwmRunning && st.ledcChannel >= 0)
            {
                // 无 DAC 时用 PWM 占空比调制模拟幅值
                float duty = (float)value / (float)(st.waveAmp > 0 ? st.waveAmp : 1);
                if (duty < 0.f) duty = 0.f;
                if (duty > 1.f) duty = 1.f;
                int d = (int)(duty * LEDC_MAX_DUTY + 0.5f);
                ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)st.ledcChannel, d);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)st.ledcChannel);
            }
        }

        if (self->m_waveMutex)
            xSemaphoreGive((SemaphoreHandle_t)self->m_waveMutex);

        if (!anyActive)
            break;  // 没有正在生成的波形, 任务退出
        vTaskDelay(period);
    }

    self->m_waveTask = nullptr;
    vTaskDelete(nullptr);
}

bool JZGpioDriverEsp32::startWaveform(JZGpio *gpio, JZGpio::Waveform type, int freqHz, int amplitude, int offset)
{
    auto it = m_pins.find(gpio->pin());
    if (it == m_pins.end())
        return false;
    PinState &st = it->second;

    if (st.useDac)
    {
        dac_channel_t ch = (gpio->pin() == 25) ? DAC_CHANNEL_1 : DAC_CHANNEL_2;
        dac_output_enable(ch);
    }
    else if (!st.pwmRunning)
    {
        // 非 DAC 引脚需要先建立一个 PWM 载波以便调制幅值
        if (!startPwm(gpio, freqHz > 1000 ? freqHz : 5000, 0.0f))
            return false;
    }

    // 在锁内设置波形参数与运行标志, 保证任务启动即能读到一致状态
    if (m_waveMutex)
        xSemaphoreTake((SemaphoreHandle_t)m_waveMutex, portMAX_DELAY);
    st.waveform = type;
    st.waveFreq = freqHz;
    st.waveAmp = amplitude;
    st.waveOffset = offset;
    st.wavePhase = 0.0;
    st.useDac = isDacPin(gpio->pin());
    st.waveRunning = true;
    if (m_waveMutex)
        xSemaphoreGive((SemaphoreHandle_t)m_waveMutex);

    ensureWaveTask();
    return true;
}

void JZGpioDriverEsp32::stopWaveform(JZGpio *gpio)
{
    auto it = m_pins.find(gpio->pin());
    if (it == m_pins.end() || !it->second.waveRunning)
        return;
    PinState &st = it->second;

    if (m_waveMutex)
        xSemaphoreTake((SemaphoreHandle_t)m_waveMutex, portMAX_DELAY);
    st.waveRunning = false;  // 任务下一轮会检测到并无波形可生成, 自动退出
    if (m_waveMutex)
        xSemaphoreGive((SemaphoreHandle_t)m_waveMutex);

    if (st.useDac)
    {
        dac_channel_t ch = (gpio->pin() == 25) ? DAC_CHANNEL_1 : DAC_CHANNEL_2;
        dac_output_disable(ch);
    }
    st.waveform = JZGpio::WaveNone;
}

// ==================== 触发 ====================
bool JZGpioDriverEsp32::setTriggerEdge(JZGpio *gpio, JZGpio::Edge edge)
{
    auto it = m_pins.find(gpio->pin());
    if (it == m_pins.end())
        return false;
    PinState &st = it->second;
    st.edge = edge;

    gpio_int_type_t t = GPIO_INTR_DISABLE;
    switch (edge)
    {
    case JZGpio::RisingEdge:  t = GPIO_INTR_POSEDGE; break;
    case JZGpio::FallingEdge: t = GPIO_INTR_NEGEDGE; break;
    case JZGpio::BothEdge:    t = GPIO_INTR_ANYEDGE; break;
    case JZGpio::NoEdge:
    default:                  t = GPIO_INTR_DISABLE; break;
    }

    if (gpio_set_intr_type((gpio_num_t)gpio->pin(), t) != ESP_OK)
        return false;

    if (edge == JZGpio::NoEdge)
    {
        if (st.isrAdded)
        {
            gpio_isr_handler_remove((gpio_num_t)gpio->pin());
            st.isrAdded = false;
        }
        return true;
    }

    if (!st.isrAdded)
    {
        if (gpio_isr_handler_add((gpio_num_t)gpio->pin(), &JZGpioDriverEsp32::isrHandler, this) != ESP_OK)
            return false;
        st.isrAdded = true;
    }
    return true;
}
