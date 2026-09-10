// TaskHistoryManager（SQLite 任务库）独立测试
//
// 覆盖：建库与自动建首任务 / 标题与消息计数 / 跨实例持久化 / 对话正文往返 /
//       删任务级联删对话 / 工程过滤 / 旧版 index.json + task_<id>.json 自动导入。
// 不依赖 GUI、大模型、网络；库建在 applicationTmpPath() 下的临时目录，跑完即删。
#include "JZTest.h"

#include "JZApplication.h"
#include "JZDir.h"
#include "JZFileInfo.h"
#include "JZPlatform.h"
#include "TaskHistoryManager.h"
#include "json/json.h"

#include <fstream>
#include <string>

using namespace std;

namespace {

const char* kWsA = "F:/mcu/JZFramework";
const char* kWsB = "F:/mcu/OtherProject";

string makeTmpRoot()
{
    string base = JZPlatform::applicationTmpPath();
    if (base.empty())
        base = ".";
    string dir = base + "/code_agent_taskstore_test";
    JZDir d(dir);
    d.removeRecursively();      // 每次从干净目录开始
    d.mkpath(dir);
    return dir;
}

void writeText(const string& path, const string& text)
{
    ofstream f(path, ios::binary | ios::trunc);
    f.write(text.data(), (streamsize)text.size());
}

// 空库首次 load：自动建一条"新任务"，库文件落地
bool taskstore_initial()
{
    string root = makeTmpRoot();

    TaskHistoryManager mgr;
    mgr.setStorageDir(root);
    mgr.setWorkspace(kWsA);
    mgr.load();

    JZ_TEST_EQ((int)mgr.tasks().size(), 1);
    JZ_TEST_TRUE(mgr.currentTaskId() > 0);
    JZ_TEST_TRUE(JZFileInfo::exists(mgr.dbPath()));

    JZDir(root).removeRecursively();
    return true;
}

// 改标题/消息数后换一个实例重读：数据必须还在（落盘到 SQLite）
bool taskstore_touch_persist()
{
    string root = makeTmpRoot();

    {
        TaskHistoryManager mgr;
        mgr.setStorageDir(root);
        mgr.setWorkspace(kWsA);
        mgr.load();

        int id = mgr.currentTaskId();
        mgr.touchTask("hello sqlite task");
        mgr.addMessage();

        const HistoryEntry* e = mgr.findEntry(id);
        JZ_TEST_TRUE(e != nullptr);
        if (e)
        {
            JZ_TEST_EQ(e->messageCount, 2);
            JZ_TEST_EQ(e->firstMessage, string("hello sqlite task"));
            JZ_TEST_TRUE(e->title.find("hello") != string::npos);
            JZ_TEST_TRUE(e->createdAt > 0);
        }
    }

    {
        TaskHistoryManager mgr;
        mgr.setStorageDir(root);
        mgr.setWorkspace(kWsA);
        mgr.load();

        JZ_TEST_EQ((int)mgr.tasks().size(), 1);
        const HistoryEntry* e = mgr.findEntry(mgr.currentTaskId());
        JZ_TEST_TRUE(e != nullptr);
        if (e)
        {
            JZ_TEST_EQ(e->messageCount, 2);
            JZ_TEST_EQ(e->firstMessage, string("hello sqlite task"));
        }
    }

    JZDir(root).removeRecursively();
    return true;
}

// 对话正文往返；删任务时级联删掉对话
bool taskstore_conversation()
{
    string root = makeTmpRoot();

    TaskHistoryManager mgr;
    mgr.setStorageDir(root);
    mgr.setWorkspace(kWsA);
    mgr.load();

    int id = mgr.currentTaskId();

    Json::Value conv;
    conv["history"][0]["role"] = "user";
    conv["history"][0]["content"] = "写一个函数";
    JZ_TEST_TRUE(mgr.saveConversation(id, conv));

    Json::Value got = mgr.conversationJson(id);
    JZ_TEST_TRUE(!got.isNull());
    JZ_TEST_EQ(got["history"][0]["content"].asString(), string("写一个函数"));

    // 不存在的任务取不到对话
    JZ_TEST_TRUE(mgr.conversationJson(id + 999).isNull());

    JZ_TEST_TRUE(mgr.deleteTask(id));
    JZ_TEST_TRUE(mgr.conversationJson(id).isNull());   // 级联删除

    // 删掉当前任务后应自动补一条空任务，界面上永远有当前任务
    JZ_TEST_EQ((int)mgr.tasks().size(), 1);

    JZDir(root).removeRecursively();
    return true;
}

// 按工程过滤 + 计数归零 + 空任务复用
bool taskstore_workspace_filter()
{
    string root = makeTmpRoot();

    TaskHistoryManager mgr;
    mgr.setStorageDir(root);
    mgr.setWorkspace(kWsA);
    mgr.load();

    mgr.touchTask("task A1");
    int aId = mgr.currentTaskId();
    int second = mgr.newTask();          // A1 有内容 -> 新建一条
    JZ_TEST_TRUE(second != aId);
    JZ_TEST_EQ((int)mgr.tasks().size(), 2);

    JZ_TEST_EQ(mgr.newTask(), second);   // 新任务还空着 -> 复用，不堆空任务
    JZ_TEST_EQ((int)mgr.tasks().size(), 2);

    // 切到另一个工程：看不到 A 的任务，且自动建一条属于 B 的
    mgr.setWorkspace(kWsB);
    mgr.load();
    JZ_TEST_EQ((int)mgr.tasks().size(), 1);
    mgr.touchTask("task B1");

    // 切回 A：两条都还在
    mgr.setWorkspace(kWsA);
    JZ_TEST_EQ((int)mgr.tasks().size(), 2);

    // 计数归零后空任务可被 newTask 复用
    mgr.switchTask(second);
    mgr.resetMessageCount();
    JZ_TEST_EQ(mgr.newTask(), second);

    JZDir(root).removeRecursively();
    return true;
}

// 旧版（index.json + task_<id>.json）一次性导入，且不删原文件
bool taskstore_legacy_import()
{
    string root = makeTmpRoot();

    Json::Value idx;
    Json::Value e;
    e["id"] = 7;
    e["title"] = "old task";
    e["firstMessage"] = "old message";
    e["createdAt"] = 1700000000000LL;
    e["messageCount"] = 3;
    e["workspace"] = kWsA;
    idx["history"].append(e);
    idx["nextTaskId"] = 8;

    Json::StyledWriter writer;
    writeText(root + "/index.json", writer.write(idx));

    Json::Value conv;
    conv["history"][0]["role"] = "user";
    conv["history"][0]["content"] = "old content";
    writeText(root + "/task_7.json", writer.write(conv));

    TaskHistoryManager mgr;
    mgr.setStorageDir(root);
    mgr.setWorkspace(kWsA);
    mgr.load();

    JZ_TEST_EQ((int)mgr.tasks().size(), 1);
    const HistoryEntry* he = mgr.findEntry(7);
    JZ_TEST_TRUE(he != nullptr);
    if (he)
    {
        JZ_TEST_EQ(he->title, string("old task"));
        JZ_TEST_EQ(he->messageCount, 3);
        JZ_TEST_EQ(he->createdAt, (int64_t)1700000000000LL);
    }

    Json::Value imported = mgr.conversationJson(7);
    JZ_TEST_TRUE(!imported.isNull());
    JZ_TEST_EQ(imported["history"][0]["content"].asString(), string("old content"));

    // 导入不删原文件（留作备份）
    JZ_TEST_TRUE(JZFileInfo::exists(root + "/index.json"));
    JZ_TEST_TRUE(JZFileInfo::exists(root + "/task_7.json"));

    // 再次 load 不应重复导入（库里已有数据）
    mgr.load();
    JZ_TEST_EQ((int)mgr.tasks().size(), 1);

    JZDir(root).removeRecursively();
    return true;
}

} // namespace

void test_TaskStore(JZTest *test)
{
    auto suite = std::make_shared<JZTestSuite>("TaskStoreTest");
    suite->addTestCase("initial", taskstore_initial);
    suite->addTestCase("touchPersist", taskstore_touch_persist);
    suite->addTestCase("conversation", taskstore_conversation);
    suite->addTestCase("workspaceFilter", taskstore_workspace_filter);
    suite->addTestCase("legacyImport", taskstore_legacy_import);
    test->addSuite(suite);
}

int main(int argc, char *argv[])
{
    JZApplication app;

    JZTest test;
    test_TaskStore(&test);
    return test.run(argc, argv);
}
