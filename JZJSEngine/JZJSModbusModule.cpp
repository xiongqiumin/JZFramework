#include "JZJSModbusModule.h"

#include <errno.h>
#include <stdlib.h>

#include "JZJSBind.h"
#include "JZModbusContext.h"
#include "JZTimer.h"

namespace
{
const int kDefaultSlave = 1;
const int kDefaultTimeoutMs = 5000;

// 本模块与 JS 侧的约定: 异步原子**只 resolve**(不 reject), 失败通过 {ok:false, message} 表达,
// JS 适配层再决定抛异常。这样避免 jsbind 对 void 异步返回吞掉 C++ 异常的问题。
Json::Value okResult()
{
    Json::Value v(Json::objectValue);
    v["ok"] = true;
    return v;
}

Json::Value errResult(const std::string &message)
{
    Json::Value v(Json::objectValue);
    v["ok"] = false;
    v["message"] = message;
    return v;
}

// Json 值 -> int(容忍 number/bool/字符串), 取不到时用 def
int jzInt(const Json::Value &v, int def)
{
    if (v.isInt() || v.isUInt() || v.isInt64() || v.isUInt64() || v.isDouble() || v.isBool())
        return v.asInt();
    if (v.isString())
    {
        std::string s = v.asString();
        const char *begin = s.c_str();
        char *end = nullptr;
        long n = strtol(begin, &end, 10);
        if (end && end != begin)
            return (int)n;
    }
    return def;
}

double jzDouble(const Json::Value &v, double def)
{
    if (v.isNumeric() || v.isBool())
        return v.asDouble();
    if (v.isString())
    {
        std::string s = v.asString();
        const char *begin = s.c_str();
        char *end = nullptr;
        double n = strtod(begin, &end);
        if (end && end != begin)
            return n;
    }
    return def;
}

std::string jzString(const Json::Value &v, const std::string &def)
{
    if (v.isString())
        return v.asString();
    return def;
}
}

//JZJSModbusClient
JZJSModbusClient::JZJSModbusClient()
{
    engine = nullptr;
    module = nullptr;
    client = nullptr;

    openHandle = -1;
    reqHandle = -1;
    reqFunc = 0;
    reqNb = 0;
    disposed = false;
}

JZJSModbusClient::~JZJSModbusClient()
{
    if (client)
    {
        client->close();
        delete client;
        client = nullptr;
    }
}

void JZJSModbusClient::dispose()
{
    if (disposed)
        return;

    disposed = true;
    openHandle = -1;
    reqHandle = -1;

    if (client)
    {
        client->close();
        delete client;
        client = nullptr;
    }
}

//JZJSModbusModule
JZJSModbusModule::JZJSModbusModule()
{
}

JZJSModbusModule::~JZJSModbusModule()
{
    deinitEngine();
}

void JZJSModbusModule::initEngine(JZJSEngine *engine)
{
    m_engine = engine;

    JSContext *ctx = m_engine->context();
    JSValue global = m_engine->globalObject();

    // 原生类: 只作为"连接句柄"(真正的操作挂在 __jz.modbus.* 上, 因为需要 promise 机制)
    jsbind::ClassBind<JZJSModbusClient> bind_client("JZJSModbusClient");
    m_engine->registClass(bind_client.classDefine());
    m_typeClient = m_engine->ctypeidToType(typeid(JZJSModbusClient).name());

    // __jz.modbus
    JSValue jz = m_engine->getProperty(global, "__jz");
    bool freeJz = !JS_IsUndefined(jz);
    if (!freeJz)
    {
        jz = m_engine->createObject();
        m_engine->setProperty(global, "__jz", jz);
    }

    JSValue ns = m_engine->createObject();
    m_engine->registFunction(ns, "create", jsbind::createSelfFunction(this, &JZJSModbusModule::create));
    m_engine->registFunction(ns, "close", jsbind::createSelfFunction(this, &JZJSModbusModule::close));
    m_engine->registFunction(ns, "isOpen", jsbind::createSelfFunction(this, &JZJSModbusModule::isOpen));
    m_engine->registFunction(ns, "setSlave", jsbind::createSelfFunction(this, &JZJSModbusModule::setSlave));
    m_engine->registFunction(ns, "setPlcMode", jsbind::createSelfFunction(this, &JZJSModbusModule::setPlcMode));
    m_engine->registFunction(ns, "lastError", jsbind::createSelfFunction(this, &JZJSModbusModule::lastError));
    m_engine->registFunction(ns, "info", jsbind::createSelfFunction(this, &JZJSModbusModule::info));

    // 异步原子(返回 Promise)
    m_engine->registAsyncFunction(ns, "open", jsbind::createSelfFunction(this, &JZJSModbusModule::open));
    m_engine->registAsyncFunction(ns, "read", jsbind::createSelfFunction(this, &JZJSModbusModule::read));
    m_engine->registAsyncFunction(ns, "write", jsbind::createSelfFunction(this, &JZJSModbusModule::write));

    m_engine->setProperty(jz, "modbus", ns);

    if (freeJz)
        JS_FreeValue(ctx, jz);
}

void JZJSModbusModule::deinitEngine()
{
    std::vector<JZJSModbusClient *> clients = m_clients;
    m_clients.clear();
    m_openTimers.clear();

    if (m_engine)
    {
        for (size_t i = 0; i < clients.size(); i++)
        {
            JZJSModbusClient *o = clients[i];
            if (!o)
                continue;

            // 引擎马上要销毁, 不用(也无法)给 JS 回结果, 只把"挂起工作计数"还回去
            if (o->openHandle >= 0)
            {
                o->openHandle = -1;
                m_engine->releasePendingWork();
            }
            if (o->reqHandle >= 0)
            {
                o->reqHandle = -1;
                m_engine->releasePendingWork();
            }

            o->dispose();
            // GC finalizer 在引擎析构时才跑, 那时本模块已不存在 -> 先断开引用
            o->module = nullptr;
            o->engine = nullptr;
        }
    }

    m_engine = nullptr;
    m_typeClient = -1;
}

JSValue JZJSModbusModule::create(const Json::Value &config)
{
    Json::Value cfg = config.isObject() ? config : Json::Value(Json::objectValue);

    std::string type = jzString(cfg["type"], "rtu");
    bool isRtu = (type == "rtu" || type == "RTU" || type == "serial" || type == "rtu-buffered");
    bool isTcp = (type == "tcp" || type == "TCP" || type == "tcp/ip");
    if (!isRtu && !isTcp)
    {
        return throwError("modbus.create: unsupported type '" + type +
                          "', expect 'rtu' or 'tcp'");
    }

    JZModbusConnetInfo info;
    info.modbusType = isRtu ? Modbus_rtuClient : Modbus_tcpClient;
    info.slave = jzInt(cfg["slave"], jzInt(cfg["unitID"], kDefaultSlave));
    info.plcMode = cfg["plcMode"].asBool();

    if (isRtu)
    {
        info.portName = jzString(cfg["portName"], jzString(cfg["path"], ""));
        if (info.portName.empty())
            return throwError("modbus.create: rtu requires 'portName' (e.g. 'COM3')");

        if (cfg.isMember("baudRate"))
            info.baud = jzInt(cfg["baudRate"], info.baud);
        else if (cfg.isMember("baud"))
            info.baud = jzInt(cfg["baud"], info.baud);

        switch (jzInt(cfg["dataBits"], 8))
        {
        case 5: info.dataBit = JZSerialPort::Data5; break;
        case 6: info.dataBit = JZSerialPort::Data6; break;
        case 7: info.dataBit = JZSerialPort::Data7; break;
        default: info.dataBit = JZSerialPort::Data8; break;
        }

        double stopBits = jzDouble(cfg["stopBits"], 1);
        if (stopBits > 1.4)
            info.stopBit = JZSerialPort::TwoStop;
        else if (stopBits > 1.1)
            info.stopBit = JZSerialPort::OneAndHalfStop;
        else
            info.stopBit = JZSerialPort::OneStop;

        std::string parity = jzString(cfg["parity"], "none");
        if (parity == "even")
            info.parityBit = JZSerialPort::EvenParity;
        else if (parity == "odd")
            info.parityBit = JZSerialPort::OddParity;
        else if (parity == "space")
            info.parityBit = JZSerialPort::SpaceParity;
        else if (parity == "mark")
            info.parityBit = JZSerialPort::MarkParity;
        else
            info.parityBit = JZSerialPort::NoParity;
    }
    else
    {
        info.ip = jzString(cfg["ip"], "127.0.0.1");
        info.port = jzInt(cfg["port"], 502);
    }

    JZJSModbusClient *o = new JZJSModbusClient();
    o->engine = m_engine;
    o->module = this;
    o->client = new JZModbusClient();
    o->client->initConn(info);

    connect(&o->client->sigModbusOpen, this, [this, o](bool ok) { this->onOpen(o, ok); });
    connect(&o->client->sigModbusReplay, this, [this, o](const JZModebusReply &reply)
            { this->onReply(o, reply); });

    m_clients.push_back(o);
    return m_engine->createValue(m_typeClient, new jsbind::JSRawPointer<JZJSModbusClient>(o, false));
}

void JZJSModbusModule::open(int promiseHandle, JZJSModbusClient *o, int timeoutMs)
{
    if (!o || !o->client)
    {
        resolveJson(promiseHandle, errResult("modbus client is not available"));
        return;
    }

    if (o->openHandle >= 0)
    {
        // 用户重连: 覆盖上一个还没结算的 open
        int old = o->openHandle;
        o->openHandle = -1;
        m_openTimers.erase(old);
        resolveJson(old, errResult("superseded by a new open() call"));
        m_engine->releasePendingWork();
    }
    else if (o->reqHandle >= 0)
    {
        resolveJson(promiseHandle, errResult("client is busy (a request is already in flight)"));
        return;
    }

    if (o->client->isOpen())
    {
        resolveJson(promiseHandle, okResult());
        return;
    }

    if (timeoutMs <= 0)
        timeoutMs = kDefaultTimeoutMs;

    o->openHandle = promiseHandle;
    m_openTimers[promiseHandle] = o;
    m_engine->addPendingWork();

    // 超时兜底: JZTcpSocket 连不上时不一定发 error(例如 IP 不可达), 不能只靠信号
    JZTimer::singleShot(timeoutMs, this, [this, promiseHandle]()
                        { this->onOpenTimeout(promiseHandle); });

    o->client->openAsync();
}

void JZJSModbusModule::read(int promiseHandle, JZJSModbusClient *o, int func, int addr, int nb)
{
    if (!o || !o->client)
    {
        resolveJson(promiseHandle, errResult("modbus client is not available"));
        return;
    }
    if (o->openHandle >= 0 || o->reqHandle >= 0)
    {
        resolveJson(promiseHandle, errResult("client is busy (a request is already in flight)"));
        return;
    }
    if (nb <= 0)
    {
        resolveJson(promiseHandle, errResult("invalid length " + std::to_string(nb)));
        return;
    }
    if (!o->client->isOpen())
    {
        resolveJson(promiseHandle, errResult("port not open"));
        return;
    }

    bool ok = false;
    switch (func)
    {
    case MODBUS_FC_READ_COILS:
        ok = o->client->readBitsAsync(addr, nb);
        break;
    case MODBUS_FC_READ_DISCRETE_INPUTS:
        ok = o->client->readInputBitsAsync(addr, nb);
        break;
    case MODBUS_FC_READ_HOLDING_REGISTERS:
        ok = o->client->readRegistersAsync(addr, nb);
        break;
    case MODBUS_FC_READ_INPUT_REGISTERS:
        ok = o->client->readInputRegistersAsync(addr, nb);
        break;
    default:
        resolveJson(promiseHandle, errResult("unsupported read function code " + std::to_string(func)));
        return;
    }

    if (!ok)
    {
        std::string err = errnoText();
        resolveJson(promiseHandle, errResult(err));
        return;
    }

    o->reqHandle = promiseHandle;
    o->reqFunc = func;
    o->reqNb = nb;
    m_engine->addPendingWork();
}

void JZJSModbusModule::write(int promiseHandle, JZJSModbusClient *o, int func, int addr,
                             JSValue value)
{
    if (!o || !o->client)
    {
        resolveJson(promiseHandle, errResult("modbus client is not available"));
        return;
    }
    if (o->openHandle >= 0 || o->reqHandle >= 0)
    {
        resolveJson(promiseHandle, errResult("client is busy (a request is already in flight)"));
        return;
    }
    if (!o->client->isOpen())
    {
        resolveJson(promiseHandle, errResult("port not open"));
        return;
    }

    JSContext *ctx = m_engine->context();

    bool ok = false;
    switch (func)
    {
    case MODBUS_FC_WRITE_SINGLE_COIL:
    {
        bool state = (JS_ToBool(ctx, value) != 0);
        ok = o->client->writeBitAsync(addr, state ? 1 : 0);
        break;
    }
    case MODBUS_FC_WRITE_SINGLE_REGISTER:
    {
        int v = 0;
        JS_ToInt32(ctx, &v, value);
        if (v < 0)
            v += 65536;  // 允许 -1 这类写法表示 0xFFFF
        ok = o->client->writeRegisterAsync(addr, (uint16_t)(v & 0xFFFF));
        break;
    }
    case MODBUS_FC_WRITE_MULTIPLE_COILS:
    case MODBUS_FC_WRITE_MULTIPLE_REGISTERS:
    {
        const char *what = (func == MODBUS_FC_WRITE_MULTIPLE_COILS) ? "coils" : "registers";
        if (!JS_IsArray(value))
        {
            resolveJson(promiseHandle, errResult(std::string("write multiple ") + what +
                                                 " expects an array"));
            return;
        }

        int64_t count = 0;
        JS_GetLength(ctx, value, &count);
        if (count <= 0)
        {
            resolveJson(promiseHandle, errResult(std::string("write multiple ") + what +
                                                 " expects a non-empty array"));
            return;
        }

        if (func == MODBUS_FC_WRITE_MULTIPLE_COILS)
        {
            std::vector<uint8_t> bits;
            bits.reserve((size_t)count);
            for (int64_t i = 0; i < count; i++)
            {
                JSValue item = JS_GetPropertyInt64(ctx, value, i);
                bits.push_back(JS_ToBool(ctx, item) ? 1 : 0);
                JS_FreeValue(ctx, item);
            }
            ok = o->client->writeBitsAsync(addr, bits);
        }
        else
        {
            std::vector<uint16_t> regs;
            regs.reserve((size_t)count);
            for (int64_t i = 0; i < count; i++)
            {
                JSValue item = JS_GetPropertyInt64(ctx, value, i);
                int v = 0;
                JS_ToInt32(ctx, &v, item);
                JS_FreeValue(ctx, item);
                if (v < 0)
                    v += 65536;  // 允许 -1 这类写法表示 0xFFFF
                regs.push_back((uint16_t)(v & 0xFFFF));
            }
            ok = o->client->writeRegistersAsync(addr, regs);
        }
        break;
    }
    default:
        resolveJson(promiseHandle, errResult("unsupported write function code " + std::to_string(func)));
        return;
    }

    if (!ok)
    {
        std::string err = errnoText();
        resolveJson(promiseHandle, errResult(err));
        return;
    }

    o->reqHandle = promiseHandle;
    o->reqFunc = func;
    o->reqNb = 1;
    m_engine->addPendingWork();
}

void JZJSModbusModule::close(JZJSModbusClient *o)
{
    if (!o || !o->client)
        return;

    // close() 会清掉在途请求(之后不会再有 reply), 必须自己把 promise 结算掉
    if (o->reqHandle >= 0)
    {
        int h = o->reqHandle;
        o->reqHandle = -1;
        m_engine->releasePendingWork();
        resolveJson(h, errResult("client closed"));
    }

    if (o->openHandle >= 0)
    {
        int h = o->openHandle;
        o->openHandle = -1;
        m_openTimers.erase(h);
        m_engine->releasePendingWork();
        resolveJson(h, errResult("client closed"));
    }

    o->client->close();
}

bool JZJSModbusModule::isOpen(JZJSModbusClient *o)
{
    return (o && o->client && o->client->isOpen());
}

bool JZJSModbusModule::setSlave(JZJSModbusClient *o, int slave)
{
    if (!o || !o->client)
        return false;

    return o->client->setSlave(slave);
}

void JZJSModbusModule::setPlcMode(JZJSModbusClient *o, bool flag)
{
    if (!o || !o->client)
        return;

    o->client->setPlcMode(flag);
}

std::string JZJSModbusModule::lastError(JZJSModbusClient *o)
{
    if (!o || !o->client)
        return "";

    return o->client->error();
}

Json::Value JZJSModbusModule::info(JZJSModbusClient *o)
{
    Json::Value v(Json::objectValue);
    if (!o || !o->client)
        return v;

    const JZModbusConnetInfo &conn = o->client->connInfo();
    v["modbusType"] = conn.modbusType;
    v["rtu"] = conn.isRtu();
    v["master"] = conn.isMaster();
    v["slave"] = conn.slave;
    v["plcMode"] = conn.plcMode;
    v["portName"] = conn.portName;
    v["baud"] = conn.baud;
    v["dataBit"] = (int)conn.dataBit;
    v["stopBit"] = (int)conn.stopBit;
    v["parityBit"] = (int)conn.parityBit;
    v["ip"] = conn.ip;
    v["port"] = conn.port;
    return v;
}

void JZJSModbusModule::onOpen(JZJSModbusClient *o, bool ok)
{
    if (!o)
        return;

    if (o->openHandle < 0)
    {
        // 非 open() 触发的信号: 已连接之后连接断开(TCP 侧 sigError)。
        // 这里没有异步通道可以主动推给 JS, 状态由 isOpen()/后续请求报错体现。
        return;
    }

    std::string err = (o->client ? o->client->error() : std::string());
    settleOpen(o, ok, ok ? std::string() : (err.empty() ? "connect failed" : "connect failed: " + err));
}

void JZJSModbusModule::onOpenTimeout(int handle)
{
    std::map<int, JZJSModbusClient *>::iterator it = m_openTimers.find(handle);
    if (it == m_openTimers.end())
        return;

    JZJSModbusClient *o = it->second;
    m_openTimers.erase(it);
    if (!o || o->openHandle != handle)
        return;

    settleOpen(o, false, "connect timed out");
}

void JZJSModbusModule::onReply(JZJSModbusClient *o, const JZModebusReply &reply)
{
    if (!o || o->reqHandle < 0)
        return;  // 超时后残留的应答 / 非本模块发起的请求

    int func = o->reqFunc;
    int nb = o->reqNb;
    int handle = o->reqHandle;
    o->reqHandle = -1;
    o->reqFunc = 0;
    o->reqNb = 0;
    m_engine->releasePendingWork();

    if (reply.code != 0)
    {
        Json::Value v = errResult(codeText(reply.code));
        v["code"] = reply.code;
        resolveJson(handle, v);
        return;
    }

    Json::Value v = okResult();
    v["function"] = func;

    if (isReadFunc(func))
    {
        Json::Value arr(Json::arrayValue);
        if (isBitFunc(func))
        {
            for (size_t i = 0; i < reply.bit.size(); i++)
                arr.append((int)reply.bit[i]);
        }
        else
        {
            for (size_t i = 0; i < reply.reg.size(); i++)
                arr.append((int)reply.reg[i]);
        }
        // 防御: 正常情况长度 == 请求数量, 异常时按请求数量截断
        if (nb > 0 && arr.size() > (Json::Value::ArrayIndex)nb)
            arr.resize(nb);
        v["data"] = arr;
    }
    else
    {
        v["address"] = reply.addr;
    }

    resolveJson(handle, v);
}

void JZJSModbusModule::settleOpen(JZJSModbusClient *o, bool ok, const std::string &message)
{
    if (!o || o->openHandle < 0)
        return;

    int handle = o->openHandle;
    o->openHandle = -1;
    m_openTimers.erase(handle);
    m_engine->releasePendingWork();

    if (ok)
    {
        resolveJson(handle, okResult());
    }
    else
    {
        resolveJson(handle, errResult(message.empty() ? "connect failed" : message));
    }
}

void JZJSModbusModule::settleRequest(JZJSModbusClient *o, bool ok, int code, const std::string &message)
{
    if (!o || o->reqHandle < 0)
        return;

    int handle = o->reqHandle;
    o->reqHandle = -1;
    o->reqFunc = 0;
    o->reqNb = 0;
    m_engine->releasePendingWork();

    Json::Value v = ok ? okResult() : errResult(message);
    if (!ok)
        v["code"] = code;
    resolveJson(handle, v);
}

JSValue JZJSModbusModule::throwError(const std::string &msg)
{
    JSContext *ctx = m_engine->context();
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, msg.c_str()));
    return JS_Throw(ctx, err);
}

void JZJSModbusModule::resolveJson(int handle, const Json::Value &value)
{
    std::vector<JZJSValuePtr> args;
    args.push_back(m_engine->toValuePtr(value));
    m_engine->setPromiseResolve(handle, args);
}

std::string JZJSModbusModule::errnoText()
{
    int e = errno;
    if (e == EMBXSBUSY)
        return "client is busy (a request is already in flight)";
    if (e == EINVAL)
        return "port not open";
    if (e == EMBMDATA)
        return "too many data (out of the allowed range)";
    return "modbus request failed (errno=" + std::to_string(e) + ")";
}

bool JZJSModbusModule::isBitFunc(int func)
{
    return func == MODBUS_FC_READ_COILS || func == MODBUS_FC_READ_DISCRETE_INPUTS;
}

bool JZJSModbusModule::isReadFunc(int func)
{
    return func == MODBUS_FC_READ_COILS || func == MODBUS_FC_READ_DISCRETE_INPUTS ||
           func == MODBUS_FC_READ_HOLDING_REGISTERS || func == MODBUS_FC_READ_INPUT_REGISTERS;
}

std::string JZJSModbusModule::codeText(int code)
{
    if (code == -1)
        return "request timed out (no reply from the slave)";
    if (code == EMBBADCRC)
        return "bad CRC";
    if (code == EMBBADDATA)
        return "bad data (message length or contents invalid)";
    if (code == EMBBADEXC)
        return "bad exception code";
    if (code == EMBUNKEXC)
        return "unknown exception code";
    if (code == EMBMDATA)
        return "too many data (out of the allowed range)";
    if (code == EMBBADSLAVE)
        return "bad slave id in the response";

    int exc = (code > MODBUS_ENOBASE) ? (code - MODBUS_ENOBASE) : code;
    static const char *kNames[] = {
        "ok",
        "illegal function",
        "illegal data address",
        "illegal data value",
        "slave device failure",
        "acknowledge",
        "slave device busy",
        "negative acknowledge",
        "memory parity error",
        "not defined",
        "gateway path unavailable",
        "gateway target failed to respond",
    };
    if (exc >= 0 && exc <= MODBUS_EXCEPTION_GATEWAY_TARGET)
        return std::string("modbus exception ") + std::to_string(exc) + ": " + kNames[exc];

    return "modbus error (code " + std::to_string(code) + ")";
}
