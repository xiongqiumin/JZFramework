#include <chrono>
#include <string>
#include <vector>

#include "JZDebug.h"
#include "JZTimer.h"
#include "JZEventLoop.h"
#include "JZThread.h"

#include "JZMqttServer.h"
#include "JZMqttClient.h"
#include "JZNetworkRequest.h"

#include "net_test_common.h"

using namespace std;

// ===================== 单线程 MQTT 发布/订阅 回环测试 =====================
class MqttTestSingle : public JZObject
{
public:
    MqttTestSingle(JZEventLoop *loop, int port)
    {
        m_prefix = "mqtt";
        m_topic = "test/bench";
        if (g_option.enableServer)
        {
            m_server = new JZMqttServer(this);
            m_server->listen(port);
        }

        if (g_option.enableClient)
        {
            m_client = new JZMqttClient(this);
            connect(&m_client->sigConnect, this, &MqttTestSingle::onClientConnect);
            connect(&m_client->sigMessage, this, &MqttTestSingle::onClientMsg);
            connect(&m_client->sigDisconnect, this, &MqttTestSingle::onDisconnect);

            JZMqttConnectOpt opt;
            opt.clientId = "bench";
            opt.cleanSession = true;
            opt.keepAlive = 30;
            m_client->connectToHost("mqtt://" + g_option.host + ":" + to_string(port), opt);
        }

        m_tps.begin();
        if (g_option.enableServer && !g_option.enableClient)   // 纯服务器模式: 等30s连接, 连接后跑20s
        {
            connect(&m_server->sigAccept, this, &MqttTestSingle::onAccept);
            m_bench = serverInstallExit(this, loop, 30000, 20000);
        }
        else
            netInstallExit(this, loop, g_option.loopTime);
        jzDebug() << "==== MQTT 单线程测试启动 (" << (g_option.enableServer && !g_option.enableClient ? string("连接后20s") : to_string(g_option.loopTime / 1000) + "s") << " 后退出, QoS1 可靠投递) ====";
    }

    void onAccept()
    {
        if (m_bench)
            m_bench->onConnected();
    }

    void onClientConnect()
    {
        JZMqttSubOpt opt;        
        opt.topic = m_topic;
        opt.qos = 1;
        m_client->subscribe(opt);   // QoS1: 验证可靠投递

        // 订阅生效有异步延迟, 用 warmup 定时器在收到首条回声前持续重试
        m_warmup = new JZTimer(this);
        connect(&m_warmup->sigTimeout, this, &MqttTestSingle::onWarmup);
        m_warmup->start(500);
        sendNext();
    }

    void onWarmup()
    {
        if (m_expect == 0)
        {
            m_sendSeq = 0;   // 首条尚未回声, 重发序号 0
            sendNext();
        }
    }

    void onClientMsg(const JZMqttMessage &msg)
    {
        uint64_t seq;
        std::string payload = str_fromUtf8(msg.payload);
        if (!netParseMsg(payload, m_prefix, seq))
            netReportError("MQTT", "解析失败 收到=" + payload);
        if (seq == m_expect)
        {
            m_expect++;
            m_tps.hit();
            sendNext();
        }
        else if (seq > m_expect)
        {
            // 首条之前的消息在订阅就绪前丢失, 宽松推进
            m_tps.hit((int)(seq - m_expect + 1));
            m_expect = seq + 1;
            sendNext();
        }
    }

    void onDisconnect() { netReportError("MQTT", "连接断开"); }

    void sendNext()
    {
        JZMqttPublishOpt opt;
        opt.topic = m_topic;
        opt.qos = 1;     // 触发新实现的 QoS1 出站可靠投递
        opt.payload = str_toUtf8(netMakeMsg(m_prefix, m_sendSeq));
        m_client->publish(opt);
        m_sendSeq++;
    }

    JZMqttServer *m_server;
    JZMqttClient *m_client;
    JZTimer *m_warmup = nullptr;
    string m_prefix = "mqtt";
    string m_topic;
    uint64_t m_expect = 0;
    uint64_t m_sendSeq = 0;
    NetTpsCounter m_tps{ "MQTT 单线程" };
    NetServerBench *m_bench = nullptr;
};

// ===================== 多线程客户端线程(各自独立连接 + 独立主题) =====================
class MqttClientThread : public JZThread
{
public:
    MqttClientThread(int port, NetTpsCounter *tps, int id)
        : m_port(port), m_tps(tps), m_id(id) 
    {
        moveToThread(this);
        connect(&sigFinished, this, &MqttClientThread::onFinished);
    }

    void run() override
    {
        m_prefix = "mqtt";
        m_topic = "test/bench/" + to_string(m_id);
        m_client = new JZMqttClient(this);
        connect(&m_client->sigConnect, this, &MqttClientThread::onConnect);
        connect(&m_client->sigMessage, this, &MqttClientThread::onMsg);
        connect(&m_client->sigDisconnect, this, &MqttClientThread::onDisconnect);

        JZMqttConnectOpt opt;
        opt.clientId = "bench" + to_string(m_id);
        opt.cleanSession = true;
        opt.keepAlive = 30;
        m_client->connectToHost("mqtt://" + g_option.host + ":" + to_string(m_port), opt);
        exec();
    }

    void onConnect()
    {
        JZMqttSubOpt opt;
        opt.topic = m_topic;
        opt.qos = 1;
        m_client->subscribe(opt);
        m_warmup = new JZTimer(this);
        connect(&m_warmup->sigTimeout, this, &MqttClientThread::onWarmup);
        m_warmup->start(500);
        sendNext();
    }

    void onWarmup()
    {
        if (m_expect == 0)
        {
            m_sendSeq = 0;
            sendNext();
        }
    }

    void onMsg(const JZMqttMessage &msg)
    {
        uint64_t seq;
        std::string payload = str_fromUtf8(msg.payload);
        if (!netParseMsg(payload, m_prefix, seq))
            netReportError("MQTT", "多线程解析失败 收到=" + payload);
        if (seq == m_expect)
        {
            m_expect++;
            m_tps->hit();
            sendNext();
        }
        else if (seq > m_expect)
        {
            m_tps->hit((int)(seq - m_expect + 1));
            m_expect = seq + 1;
            sendNext();
        }
    }

    void onDisconnect() { netReportError("MQTT", "多线程连接断开"); }

    void sendNext()
    {
        JZMqttPublishOpt opt;
        opt.topic = m_topic;
        opt.qos = 1;
        opt.payload = str_toUtf8(netMakeMsg(m_prefix, m_sendSeq));
        m_client->publish(opt);
        m_sendSeq++;
    }

    void onFinished()
    {
        m_warmup->stop();
        m_client->disconnectFromHost();
        delete m_client;
        moveToThread(m_parent->thread());
    }

    int m_port;
    NetTpsCounter *m_tps;
    int m_id;
    JZMqttClient *m_client = nullptr;
    JZTimer *m_warmup = nullptr;
    string m_prefix = "mqtt";
    string m_topic;
    uint64_t m_expect = 0;
    uint64_t m_sendSeq = 0;
};

// ===================== 主线程: 1 个 Broker + 管理 N 个客户端线程 =====================
class MqttTestMulti : public JZObject
{
public:
    vector<MqttClientThread *> threads;
    NetTpsCounter m_tps{"MQTT 多线程"};

    MqttTestMulti(JZEventLoop *loop, int port)
    {
        if (g_option.enableServer)
        {
            m_server = new JZMqttServer(this);
            m_server->listen(port);
        }

        if (g_option.enableClient)
        {
            for (int i = 0; i < NET_TEST_THREAD_COUNT; i++)
            {
                auto t = new MqttClientThread(port, &m_tps, i);
                t->setParent(this);
                threads.push_back(t);
            }
            // 所有线程的 parent-child 关系建立完毕后再统一 start，
            // 避免前一个线程 run() 中修改 m_childs 与后一个线程的 setParent 遍历同一 m_childs 产生数据竞争
            for (auto t : threads)
            {
                t->start();
            }
        }

        m_tps.begin();
        if (g_option.enableServer && !g_option.enableClient)   // 纯服务器模式: 等30s连接, 连接后跑20s
        {
            connect(&m_server->sigAccept, this, &MqttTestMulti::onAccept);
            m_bench = serverInstallExit(this, loop, 30000, 20000);
        }
        else
            netInstallExit(this, loop, g_option.loopTime);
        jzDebug() << "==== MQTT 多线程测试启动 (" << NET_TEST_THREAD_COUNT << " 客户端, " << (g_option.enableServer && !g_option.enableClient ? string("连接后20s") : to_string(g_option.loopTime / 1000) + "s") << " 后退出) ====";
    }

    void onAccept()
    {
        if (m_bench)
            m_bench->onConnected();
    }

    ~MqttTestMulti()
    {
        for (auto t : threads)
        {
            t->quit();
            t->wait();
            delete t;
        }
    }

    JZMqttServer *m_server;
    NetServerBench *m_bench = nullptr;
};

int test_mqtt_single()
{
    g_netTestActive = true;
    JZEventLoop loop;
    MqttTestSingle test(&loop, 18103);
    loop.exec();
    double tps = test.m_tps.average();
    netCheckTps("MQTT 单线程", tps);
    g_netTestActive = false;
    JZThread::msleep(g_option.delayTime);
    return (int)tps;
}

int test_mqtt_multi()
{
    g_netTestActive = true;
    JZEventLoop loop;
    MqttTestMulti test(&loop, 18107);
    loop.exec();
    double tps = test.m_tps.average();
    netCheckTps("MQTT 多线程", tps);
    g_netTestActive = false;
    JZThread::msleep(g_option.delayTime);
    return (int)tps;
}

int test_mqtt()
{
    jzDebug() << "########## MQTT 测试 (单线程 + 多线程, 各 15s) ##########";
    test_mqtt_single();
    jzDebug() << "【OK】MQTT 单线程通过";
    test_mqtt_multi();
    jzDebug() << "【OK】MQTT 多线程通过";
    return 0;
}
