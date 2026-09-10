#ifndef JS_ENGINE_MODBUS_MODULE_H_
#define JS_ENGINE_MODBUS_MODULE_H_

#include <map>
#include <string>
#include <vector>

#include "JZJSEngine.h"
#include "JZModbusClient.h"

class JZJSModbusModule;

// 一个 Modbus 主站连接(JS 侧 ModbusRTU / ModbusTCP 实例的底层对象)。
//
// 生命周期: 由 JS 对象拥有(jsbind ClassBind -> GC finalizer), 用户不显式释放;
//          引擎销毁时 JZJSModbusModule::deinitEngine() 会先 dispose() 一次
//          (结算在途 promise + 关连接 + 杀定时器), 之后的 GC finalizer 只做兜底。
//
// 线程: 全部操作都在 JS 引擎所在线程(m_engine 的线程)上执行, 不需要加锁。
struct JZJSModbusClient
{
    JZJSModbusClient();
    ~JZJSModbusClient();

    // 幂等: 关闭并释放底层连接(之后 client == nullptr, 任何请求都会被拒绝)
    void dispose();

    JZJSEngine* engine;             // 构造 JS 值 / 结算 promise 用(引擎销毁前可能被置空)
    JZJSModbusModule* module;       // 归属模块(引擎销毁前可能被置空)
    JZModbusClient* client;         // 底层连接(nullptr = 已释放)

    int openHandle;                 // open() 在途 promise handle, -1 表示无
    int reqHandle;                  // 读/写在途 promise handle, -1 表示无
    int reqFunc;                    // 在途请求的功能码(用于组织返回值)
    int reqNb;                      // 在途请求的数据个数
    bool disposed;
};

// Modbus 模块: 注册 __jz.modbus 原子 API + 原生类 JZJSModbusClient。
//
// 设计要点(与 JZJSSqliteModule 一致的分层):
//   C++ 侧只做"原子操作", 且**只覆盖 JZModbus 现有的能力**(RTU/TCP 主站,
//   FC1/2/3/4/5/6/15/16, 从站号 + PLC 模式);
//   Node 风格封装(modbus-serial 的 ModbusRTU / ModbusTCP 接口、EventEmitter、
//   请求排队、setTimeout 守卫)在 JZJSEngine/js_wrapper/modbus/index.ts, 由用户
//   入口 import './modbus/dist/index.js' 被动加载。
//
// 为什么 I/O 全是异步(registAsyncFunction):
//   JZModbusClient 的同步收发靠"泵事件循环 + 阻塞等待"实现, 在 JS 回调里直接调用
//   会把 quickjs 重入(JS -> C++ -> processEvents -> JS)。所以这里一律走
//   openAsync()/xxxAsync() + sigModbusOpen / sigModbusReplay 异步回填 promise。
class JZJSModbusModule : public JZObject
{
public:
    JZJSModbusModule();
    ~JZJSModbusModule();

    void initEngine(JZJSEngine* engine);
    void deinitEngine();

    // ---- 原子 API(挂在 __jz.modbus 下) ----
    // 建连接对象。config: {type:'rtu'|'tcp', slave, plcMode,
    //                     portName, baudRate, dataBits, stopBit, parity, ip, port}
    JSValue create(const Json::Value& config);

    // 异步: 打开连接(openAsync + 超时兜底)
    void open(int promiseHandle, JZJSModbusClient* o, int timeoutMs);

    // 异步: 读(func = 1/2/3/4, nb = 数量)
    void read(int promiseHandle, JZJSModbusClient* o, int func, int addr, int nb);

    // 异步: 写(func = 5/6/15/16, value = 单值(number/bool) 或 数组)
    // 注意: value 必须是 JSValue 而不是 Json::Value —— jsbind 的参数类型检查只允许
    // Json::Value 接收"对象/数组"(JSType_jsonValue), 传 number/bool 会在调用前被拒。
    void write(int promiseHandle, JZJSModbusClient* o, int func, int addr,
               JSValue value);

    void close(JZJSModbusClient* o);
    bool isOpen(JZJSModbusClient* o);
    bool setSlave(JZJSModbusClient* o, int slave);
    void setPlcMode(JZJSModbusClient* o, bool flag);
    std::string lastError(JZJSModbusClient* o);
    Json::Value info(JZJSModbusClient* o);

protected:
    // ---- 信号回调 ----
    void onOpen(JZJSModbusClient* o, bool ok);          // sigModbusOpen
    void onReply(JZJSModbusClient* o, const JZModebusReply& reply);  // sigModbusReplay
    void onOpenTimeout(int handle);                     // open() 的超时兜底

    // ---- 内部工具 ----
    // 抛 JS Error 并返回 JS_EXCEPTION(调用方直接 return)
    JSValue throwError(const std::string& msg);
    // 用 Json::Value 结算一个 promise(本模块约定: 只 resolve, 失败信息放 ok/message)
    void resolveJson(int handle, const Json::Value& value);
    void settleOpen(JZJSModbusClient* o, bool ok, const std::string& message);
    void settleRequest(JZJSModbusClient* o, bool ok, int code, const std::string& message);
    // errno(EINVAL/EMBXSBUSY/EMBMDATA) -> 人类可读文本
    std::string errnoText();
    // 功能码 -> 是否"读位"(FC1/2)
    static bool isBitFunc(int func);
    static bool isReadFunc(int func);
    // modbus 异常码 -> 文本
    static std::string codeText(int code);

    JZJSEngine* m_engine = nullptr;
    int m_typeClient = -1;
    std::vector<JZJSModbusClient*> m_clients;      // 本模块创建的全部连接(引擎销毁时兜底)
    std::map<int, JZJSModbusClient*> m_openTimers; // openHandle -> 连接(超时兜底用)
};

#endif  // !JS_ENGINE_MODBUS_MODULE_H_
