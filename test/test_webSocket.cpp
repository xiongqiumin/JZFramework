#include <atomic>
#include "JZApplication.h"
#include "JZCo.h"
#include "JZDebug.h"
#include "JZWebSocketServer.h"
#include "JZWebSocket.h"
#include "JZDebug.h"
#include "JZTimer.h"

class JZWebSocketTest : public JZObject
{
public:
	std::atomic<int> msgCount = 0;
	std::chrono::steady_clock::time_point startTime;

	// 回环校验：消息序号
	std::atomic<uint64_t> send_seq = 0;		  // 客户端发送序号
	std::atomic<uint64_t> expect_recv_seq = 0; // 期望接收的序号

	JZWebSocketTest()
	{
        int port = 18000;

		server = new JZWebSocketServer(this);
		server->listen(port);
		connect(&server->sigAccept, this, &JZWebSocketTest::onServerAccecpt);

		client = new JZWebSocket(this);
		client->connectToHost(JZNetworkRequest("ws://127.0.0.1:" + to_string(port)));

		startTPSCount();

		connect(&client->sigConnected, this, &JZWebSocketTest::onClientConnect);
		connect(&client->sigTextMessageReceived, this, &JZWebSocketTest::onClientRecv);
	}

	void startTPSCount()
	{
		startTime = std::chrono::steady_clock::now();

		JZTimer* tpsTimer = new JZTimer(this); 
		connect(&tpsTimer->sigTimeout, this, &JZWebSocketTest::printTPS);
		tpsTimer->start(1000);
	}

	void printTPS()
	{
		auto now = std::chrono::steady_clock::now();
		double elapsed = std::chrono::duration<double>(now - startTime).count();
		int count = msgCount.exchange(0);
		double tps = count / elapsed;
		jzDebug() << "当前 TPS：" << tps;
		startTime = now;
	}

	void onServerAccecpt()
	{
		server_socket = server->nextPendingConnection();
		connect(&server_socket->sigTextMessageReceived, this, &JZWebSocketTest::onServerRecv);
	}

	void onServerRecv(const string& text)
	{
		// 服务器原样回传
		server_socket->sendTextMessage(text);
	}

	// ===================== 校验失败直接退出 =====================
	void onClientRecv(const string &text)
	{
		string prefix = "hello world ";
		if (text.substr(0, prefix.size()) != prefix) {
			// 格式错误，直接退出
			jzDebug() << "【校验失败】非法消息格式：" << text;
			jzDebug() << "程序退出...";
			exit(-1); // 退出进程
		}

		// 解析序号
		string num_str = text.substr(prefix.size());
		uint64_t recv_seq = stoull(num_str);

		// 序号校验
		if (recv_seq != expect_recv_seq) {
			jzDebug() << "【校验失败】期望序号：" << expect_recv_seq 
				 << " 实际收到：" << recv_seq;
			jzDebug() << "消息丢失/乱序，程序退出...";
			exit(-1); // 退出进程
		}

		// 校验通过
		expect_recv_seq++;

		// 发送下一条
		uint64_t current_seq = send_seq.fetch_add(1);
		string send_msg = "hello world " + to_string(current_seq);
		client->sendTextMessage(send_msg);

		// TPS 统计
		msgCount++;
	}

	void onClientConnect()
	{
		// 发送第一条消息
		uint64_t current_seq = send_seq.fetch_add(1);
		string send_msg = "hello world " + to_string(current_seq);
		client->sendTextMessage(send_msg);
	}

	JZWebSocketServer* server;
	JZWebSocket* client;
	JZWebSocket* server_socket;
};

// ===================== 客户端线程（独立运行） =====================
class WebSocketClientThread : public JZThread
{
public:
    JZWebSocket* client = nullptr;
    string serverUrl;
    int threadId = 0;

public:
    WebSocketClientThread(const string& url, int id) : serverUrl(url), threadId(id) {}

    void run() override {
        client = new JZWebSocket();

        connect(&client->sigTextMessageReceived, this, [this](const string& text) {
            sendMessage(text);
        });

        client->connectToHost(JZNetworkRequest(serverUrl));
        exec(); // 线程事件循环
    }

    void sendMessage(const string& text) {
        if (client) client->sendTextMessage(text);
    }
};

// ===================== 主线程：Server + 管理8个客户端线程 =====================
class JZWebSocketTestMultiThread : public JZObject
{
public:
    std::atomic<int> msgCount = 0;
    std::chrono::steady_clock::time_point startTime;

    // 8个客户端线程
    static const int CLIENT_THREAD_COUNT = 16;
    std::vector<WebSocketClientThread*> clientThreads;

    // 全局序号校验
    std::atomic<uint64_t> send_seq = 0;
    std::map<JZWebSocket*,uint64_t> expect_recv_map;

    JZWebSocketServer* server = nullptr;
    int port = 18000;

public:
    JZWebSocketTestMultiThread() {
        // ========== 1. 主线程启动 Server ==========
        server = new JZWebSocketServer(this);
        server->listen(port);
        connect(&server->sigAccept, this, &JZWebSocketTestMultiThread::onServerAccept);

        // ========== 2. 启动 8 个客户端线程 ==========
        string url = "ws://127.0.0.1:" + to_string(port);
        for (int i = 0; i < CLIENT_THREAD_COUNT; i++) {
            auto thread = new WebSocketClientThread(url, i);
            thread->start();
            clientThreads.push_back(thread);
        }

        // ========== 3. TPS 统计 ==========
        startTPSCount();
    }

    ~JZWebSocketTestMultiThread() {
        for (auto t : clientThreads) {
            t->quit();
            t->wait();
        }
    }

    void startTPSCount() {
        startTime = std::chrono::steady_clock::now();
        JZTimer* tpsTimer = new JZTimer(this);
        connect(&tpsTimer->sigTimeout, this, &JZWebSocketTestMultiThread::printTPS);
        tpsTimer->start(1000);
    }

    void printTPS() {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - startTime).count();
        int count = msgCount.exchange(0);
        double tps = count / elapsed;
        jzDebug() << "【8线程并发】当前 TPS：" << tps;
        startTime = now;
    }

    // ===================== 服务端接收 & 回传 =====================
    void onServerAccept() 
    {    
        JZWebSocket* socket = server->nextPendingConnection();
        if (!socket)
            return;

        connect(&socket->sigConnected, this, &JZWebSocketTestMultiThread::onClientDisConnect);
        connect(&socket->sigTextMessageReceived, this, &JZWebSocketTestMultiThread::onClientRecv);

        uint64_t curr = send_seq.fetch_add(1);
        expect_recv_map[socket] = curr;

        string msg = "hello world " + to_string(curr);
        socket->sendTextMessage(msg);
    }

    void onClientDisConnect()
    {
        jzDebug() << "测试连接中断";
        exit(-1);
    }

    // ===================== 接收消息 + 全局校验（失败立即退出） =====================
    void onClientRecv(const string& text) 
    {
        static const string prefix = "hello world ";
        auto socket = (JZWebSocket*)sender();

        // 格式校验
        if (text.compare(0, prefix.size(), prefix) != 0) {
            jzDebug() << "校验失败：非法格式";
            exit(-1);
        }

        int expect_recv_seq = expect_recv_map[socket];
        // 序号校验
        uint64_t recv_seq = stoull(text.substr(prefix.size()));
        if (recv_seq != expect_recv_seq) {
            jzDebug() << "校验失败！期望："
                      << expect_recv_seq << " 实际：" << recv_seq;
            exit(-1);
        }

        // 发送下一条
        uint64_t curr = send_seq.fetch_add(1);
        expect_recv_map[socket] = curr;

        string sendMsg = prefix + to_string(curr);
        socket->sendTextMessage(sendMsg);

        msgCount++;
    }
};

int main()
{
    JZApplication app;

    JZWebSocketTest test;
	JZWebSocketTestMultiThread test_multi;

	return app.exec();
}