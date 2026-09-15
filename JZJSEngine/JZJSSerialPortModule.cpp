#include "JZJSSerialPortModule.h"

#include <algorithm>
#include <string>

#include "JZDebug.h"
#include "JZJSBind.h"

// ======================================================================
// 工具
// ======================================================================
static bool jsSameValue(JSValue a, JSValue b)
{
    return a.u.ptr == b.u.ptr && JS_VALUE_GET_TAG(a) == JS_VALUE_GET_TAG(b);
}

static JSValue jsMakeUint8Array(JSContext* ctx, const JZByteArray& data)
{
    return JS_NewUint8ArrayCopy(ctx, (const uint8_t*)data.data(), data.size());
}

static bool jsValueToBytes(JSContext* ctx, JSValue v, JZByteArray& out)
{
    out.clear();
    if (JS_IsString(v))
    {
        size_t len = 0;
        const char* p = JS_ToCStringLen(ctx, &len, v);
        if (!p)
            return false;
        out = JZByteArray(p, (int)len);
        JS_FreeCString(ctx, p);
        return true;
    }

    size_t size = 0;
    uint8_t* buf = JS_GetUint8Array(ctx, &size, v);
    if (buf)
    {
        out = JZByteArray((const char*)buf, (int)size);
        return true;
    }

    if (JS_IsArrayBuffer(v))
    {
        size_t ab_size = 0;
        uint8_t* ab = JS_GetArrayBuffer(ctx, &ab_size, v);
        if (ab)
        {
            out = JZByteArray((const char*)ab, (int)ab_size);
            return true;
        }
    }
    return false;
}

static JZSerialPort::Parity toParity(const std::string& p)
{
    if (p == "even")   return JZSerialPort::EvenParity;
    if (p == "odd")    return JZSerialPort::OddParity;
    if (p == "space")  return JZSerialPort::SpaceParity;
    if (p == "mark")   return JZSerialPort::MarkParity;
    return JZSerialPort::NoParity;
}

static JZSerialPort::FlowControl toFlowControl(const std::string& f)
{
    if (f == "hardware") return JZSerialPort::HardwareControl;
    if (f == "software") return JZSerialPort::SoftwareControl;
    return JZSerialPort::NoFlowControl;
}

static JZSerialPort::StopBits toStopBits(double v)
{
    if (v == 1.5) return JZSerialPort::OneAndHalfStop;
    if (v == 2)   return JZSerialPort::TwoStop;
    return JZSerialPort::OneStop;
}

// ======================================================================
// JZJSSerialPort
// ======================================================================
class JZJSSerialPort
{
public:
    JZJSSerialPort(JZJSEngine* engine = nullptr, JZJSSerialPortModule* module = nullptr)
        : engine(engine), module(module), port(nullptr), disposed(false)
    {
    }

    ~JZJSSerialPort()
    {
        close();
        auto it = std::find(module->m_ports.begin(), module->m_ports.end(), this);
        if (it != module->m_ports.end())
            module->m_ports.erase(it);
        clearListeners();
    }

    void applyOptions(const Json::Value& opts)
    {
        if (!port)
            return;
        if (opts["path"].isString())
            port->setPortName(opts["path"].asString());
        if (opts["baudRate"].isNumeric())
            port->setBaudRate(opts["baudRate"].asInt());
        if (opts["dataBits"].isNumeric())
            port->setDataBits((JZSerialPort::DataBits)opts["dataBits"].asInt());
        if (opts["stopBits"].isNumeric())
            port->setStopBits(toStopBits(opts["stopBits"].asDouble()));
        if (opts["parity"].isString())
            port->setParity(toParity(opts["parity"].asString()));
        if (opts["flowControl"].isString())
            port->setFlowControl(toFlowControl(opts["flowControl"].asString()));
        if (opts["dtr"].isBool())
            port->setDtr(opts["dtr"].asBool());
        if (opts["rts"].isBool())
            port->setRts(opts["rts"].asBool());
    }

    bool open()
    {
        if (!port)
            return false;
        return port->open();
    }

    void close()
    {
        if (disposed)
            return;
        disposed = true;
        emit("close");
        if (port)
        {
            JZSerialPort* p = port;
            port = nullptr;
            p->close();
            delete p;
        }
    }

    bool isOpen() const { return port && port->isOpen(); }

    bool write(JSValue data)
    {
        if (!port || !port->isOpen())
            return false;
        JZByteArray buf;
        if (!jsValueToBytes(engine->context(), data, buf))
            return false;
        port->write(buf);
        return true;
    }

    JSValue read(int maxSize)
    {
        JSContext* ctx = engine ? engine->context() : nullptr;
        if (!ctx || !port)
            return JS_NewUint8ArrayCopy(ctx, nullptr, 0);
        return jsMakeUint8Array(ctx, port->read(maxSize));
    }

    JSValue readAll()
    {
        JSContext* ctx = engine ? engine->context() : nullptr;
        if (!ctx || !port)
            return JS_NewUint8ArrayCopy(ctx, nullptr, 0);
        return jsMakeUint8Array(ctx, port->readAll());
    }

    bool setBaudRate(int32_t v) { return port && port->setBaudRate(v); }
    bool setDataBits(int v) { return port && port->setDataBits((JZSerialPort::DataBits)v); }
    bool setParity(const std::string& p) { return port && port->setParity(toParity(p)); }
    bool setStopBits(double v) { return port && port->setStopBits(toStopBits(v)); }
    bool setFlowControl(const std::string& f) { return port && port->setFlowControl(toFlowControl(f)); }
    bool setDtr(bool v) { return port && port->setDtr(v); }
    bool setRts(bool v) { return port && port->setRts(v); }

    // ---- 事件 ----
    void on(const std::string& event, JSValue cb)
    {
        JSContext* ctx = engine ? engine->context() : nullptr;
        if (!ctx || !JS_IsFunction(ctx, cb))
            return;
        int id = module->retainCallback(cb);
        m_listeners[event].push_back(std::make_pair(id, JS_DupValue(ctx, cb)));
    }

    void off(const std::string& event, JSValue cb)
    {
        JSContext* ctx = engine ? engine->context() : nullptr;
        auto it = m_listeners.find(event);
        if (it == m_listeners.end())
            return;

        if (JS_IsNull(cb) || JS_IsUndefined(cb))
        {
            clearEvent(event);
            return;
        }

        auto& list = it->second;
        for (size_t i = 0; i < list.size();)
        {
            if (jsSameValue(list[i].second, cb))
            {
                module->releaseCallback(list[i].first, list[i].second);
                list.erase(list.begin() + i);
            }
            else
            {
                i++;
            }
        }
        if (list.empty())
            m_listeners.erase(it);
    }

    void emit(const std::string& event) { std::vector<JSValue> a; emitArgs(event, a); }
    void emit(const std::string& event, JSValue a1) { std::vector<JSValue> a; a.push_back(a1); emitArgs(event, a); }

    JZJSEngine* engine;
    JZJSSerialPortModule* module;
    JZSerialPort* port;
    bool disposed;
    std::map<std::string, std::vector<std::pair<int, JSValue>>> m_listeners;

    void clearListeners()
    {
        for (auto& kv : m_listeners)
            for (auto& p : kv.second)
                module->releaseCallback(p.first, p.second);
        m_listeners.clear();
    }

private:
    void emitArgs(const std::string& event, std::vector<JSValue>& argv)
    {
        JSContext* ctx = engine ? engine->context() : nullptr;
        if (!ctx)
        {
            for (auto& v : argv) JS_FreeValue(ctx, v);
            return;
        }
        auto it = m_listeners.find(event);
        if (disposed || it == m_listeners.end() || it->second.empty())
        {
            for (auto& v : argv) JS_FreeValue(ctx, v);
            return;
        }

        std::vector<JSValue> list;
        for (auto& p : it->second)
            list.push_back(p.second);
        for (auto& v : list)
        {
            JSValue ret = JS_Call(ctx, v, JS_UNDEFINED, (int)argv.size(), argv.data());
            if (JS_IsException(ret))
            {
                JSValue exc = JS_GetException(ctx);
                JS_FreeValue(ctx, exc);
            }
            JS_FreeValue(ctx, ret);
        }
        for (auto& v : argv) JS_FreeValue(ctx, v);
    }

    void clearEvent(const std::string& event)
    {
        auto it = m_listeners.find(event);
        if (it == m_listeners.end())
            return;
        for (auto& p : it->second)
            module->releaseCallback(p.first, p.second);
        m_listeners.erase(it);
    }
};

// ======================================================================
// JZJSSerialPortModule
// ======================================================================
JZJSSerialPortModule::JZJSSerialPortModule()
{
    m_engine = nullptr;
    m_cbRoot = JS_UNDEFINED;
    m_cbId = 0;
}

JZJSSerialPortModule::~JZJSSerialPortModule()
{
    deinitEngine();
}

int JZJSSerialPortModule::retainCallback(JSValue cb)
{
    JSContext* ctx = m_engine->context();
    int id = m_cbId++;
    // 根数组持有引用, 使闭包对 GC 始终可达(否则"只被 C++ 持有的闭包"会被回收)
    JS_SetPropertyInt64(ctx, m_cbRoot, id, JS_DupValue(ctx, cb));
    return id;
}

void JZJSSerialPortModule::releaseCallback(int id, JSValue value)
{
    JSContext* ctx = m_engine->context();
    if (id >= 0)
        JS_SetPropertyInt64(ctx, m_cbRoot, id, JS_UNDEFINED);
    JS_FreeValue(ctx, value);
}

void JZJSSerialPortModule::initEngine(JZJSEngine* engine)
{
    m_engine = engine;
    auto global = m_engine->globalObject();

    // 回调 GC 根数组(挂到全局, 防止"只被 C++ 引用"的闭包被 GC 回收)
    m_cbRoot = JS_NewArray(m_engine->context());
    m_engine->setProperty(global, "__jzSerialPortCbRoot", JS_DupValue(m_engine->context(), m_cbRoot));

    // ---- SerialPort 类 ----
    jsbind::ClassBind<JZJSSerialPort> bind_port("JZJSSerialPort");
    bind_port.def("open", [](JZJSSerialPort* o) -> bool { return o->open(); });
    bind_port.def("close", [](JZJSSerialPort* o) { o->close(); });
    bind_port.def("write", [](JZJSSerialPort* o, JSValue data) -> bool { return o->write(data); });
    bind_port.def("read", [](JZJSSerialPort* o, int maxSize) -> JSValue { return o->read(maxSize); });
    bind_port.def("readAll", [](JZJSSerialPort* o) -> JSValue { return o->readAll(); });
    bind_port.def("setBaudRate", [](JZJSSerialPort* o, int v) -> bool { return o->setBaudRate(v); });
    bind_port.def("setDataBits", [](JZJSSerialPort* o, int v) -> bool { return o->setDataBits(v); });
    bind_port.def("setParity", [](JZJSSerialPort* o, const std::string& v) -> bool { return o->setParity(v); });
    bind_port.def("setStopBits", [](JZJSSerialPort* o, double v) -> bool { return o->setStopBits(v); });
    bind_port.def("setFlowControl", [](JZJSSerialPort* o, const std::string& v) -> bool { return o->setFlowControl(v); });
    bind_port.def("setDtr", [](JZJSSerialPort* o, bool v) -> bool { return o->setDtr(v); });
    bind_port.def("setRts", [](JZJSSerialPort* o, bool v) -> bool { return o->setRts(v); });
    bind_port.def("on", [](JZJSSerialPort* o, const std::string& e, JSValue cb) { o->on(e, cb); });
    bind_port.def("off", [](JZJSSerialPort* o, const std::string& e, JSValue cb) { o->off(e, cb); });
    bind_port.defProperty<std::string>("portName",
        [](JZJSSerialPort* o) { return o->port ? o->port->portName() : std::string(); }, nullptr);
    bind_port.defProperty<bool>("isOpen", [](JZJSSerialPort* o) { return o->isOpen(); }, nullptr);
    m_engine->registClass(bind_port.classDefine());
    m_typePort = m_engine->ctypeidToType(typeid(JZJSSerialPort).name());

    // ---- __jz.serialport 命名空间(复用已存在的 __jz, 不覆盖其它子命名空间) ----
    JSValue jz = m_engine->getProperty(global, "__jz");
    bool jzFromGet = !JS_IsUndefined(jz);
    if (!jzFromGet)
    {
        jz = m_engine->createObject();
        m_engine->setProperty(global, "__jz", jz);
    }

    JSValue ns = m_engine->createObject();
    m_engine->registFunction(ns, "list",
                             jsbind::createSelfFunction(this, &JZJSSerialPortModule::serialPortList));
    m_engine->registFunction(ns, "createPort",
                             jsbind::createSelfFunction(this, &JZJSSerialPortModule::serialPortCreate));
    m_engine->setProperty(jz, "serialport", ns);

    if (jzFromGet)
        JS_FreeValue(m_engine->context(), jz);   // 释放 getProperty 多 dup 的引用

    // ---- JS 适配层不再在 initEngine 主动加载 ----
    // 与 net/core 一致: serialport 适配层(dist/index.js)由用户入口模块
    // `import { SerialPort } from './serialport/dist/index.js'` 被动触发,
    // QuickJS 模块加载器解析到该 specifier 时会回调 readModuleCode 加载并执行它。
    // 本函数只负责注册 C++ 原子 API(__jz.serialport), 适配层包装后挂到 globalThis。
}

void JZJSSerialPortModule::deinitEngine()
{
    if (!m_engine)
        return;

    // 统一关闭底层串口资源(避免把 delete 串口对象留到 GC finalizer 里做),
    // 同时解绑所有回调。wrapper 本身由 JS GC 拥有, 此处不 delete。
    for (size_t i = 0; i < m_ports.size(); i++)
    {
        m_ports[i]->close();
        m_ports[i]->clearListeners();
    }
    m_ports.clear();

    JSContext* ctx = m_engine->context();
    if (ctx)
    {
        JS_SetPropertyStr(ctx, m_engine->globalObject(), "__jzSerialPortCbRoot", JS_UNDEFINED);
        JS_FreeValue(ctx, m_cbRoot);
    }
    m_cbRoot = JS_UNDEFINED;
    m_engine = nullptr;
}

JSValue JZJSSerialPortModule::serialPortCreate(const Json::Value& options)
{
    JZJSSerialPort* obj = new JZJSSerialPort(m_engine, this);
    obj->port = new JZSerialPort();
    obj->applyOptions(options);
    m_ports.push_back(obj);

    JZJSSerialPort* self = obj;
    connect(&obj->port->sigReadyRead, [self]() {
        if (!self->port)
            return;
        JSContext* c = self->engine->context();
        JZByteArray data = self->port->readAll();
        if (data.isEmpty())
            return;
        self->emit("data", jsMakeUint8Array(c, data));
    });

    return m_engine->createValue(m_typePort, new jsbind::JSRawPointer<JZJSSerialPort>(m_engine, obj));
}

Json::Value JZJSSerialPortModule::serialPortList()
{
    auto list = JZSerialPort::availablePorts();
    Json::Value res(Json::arrayValue);
    for (size_t i = 0; i < list.size(); i++)
    {
        Json::Value item;
        item["id"] = list[i].id;
        item["portName"] = list[i].portName;
        item["description"] = list[i].description;
        res.append(item);
    }
    return res;
}
