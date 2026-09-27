#ifndef JS_ENGINE_SERIAL_PORT_MODULE_H_
#define JS_ENGINE_SERIAL_PORT_MODULE_H_
#include <map>
#include <string>
#include <vector>

#include "JZJSEngine.h"
#include "JZByteArray.h"
#include "JZSerialPort.h"
#include "json/json.h"

class JZJSSerialPort;

/**
 * JS 串口模块: 把 JZSerialPort 暴露成 Node "serialport" 包风格 API。
 * 与 JZJSNetModule / JZJSCoreModule 平级, 各自独立 initEngine/deinitEngine,
 * 通过复用全局 __jz 命名空间(若已存在则追加 serialport 子命名空间)挂接。
 */
class JZFRAMEWORK_API JZJSSerialPortModule : public JZObject
{
public:
    JZJSSerialPortModule();
    ~JZJSSerialPortModule();

    void initEngine(JZJSEngine* engine);
    void deinitEngine();

    // ---- 原子 API(由内嵌 JS 适配层调用) ----
    JSValue serialPortCreate(const Json::Value& options);
    Json::Value serialPortList();

    // 回调 GC 根: 存进根数组使闭包始终可达, 释放时按 id 移除
    int retainCallback(JSValue cb);
    void releaseCallback(int id, JSValue value);

private:
    JSValue makePortValue(JZJSSerialPort* obj);

    JZJSEngine* m_engine = nullptr;
    int m_typePort = -1;
    JSValue m_cbRoot;
    int m_cbId = 0;
    std::vector<JZJSSerialPort*> m_ports;   // 仅用于引擎销毁时统一关闭底层资源

    friend class JZJSSerialPort;
};

#endif  // !JS_ENGINE_SERIAL_PORT_MODULE_H_
