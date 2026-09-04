// 五子棋综合测试: 一个 MCP 服务器(带五子棋仲裁) + 两个玩家通过 MCP 工具对弈。
//
//   --mode single  单 agent: 大模型(黑) vs 本地机器人(白)      [默认]
//   --mode dual    双 agent: 大模型A(黑) vs 大模型B(白)
//
// 完整流程: 黑方 create_room -> 白方 list_rooms + join_room -> 双方轮流 play
//           -> 仲裁判出胜负 -> 打印最终棋盘。
//
// 其它参数: --port(默认 3900) --modelUrl --modelA --modelB

#include <string>
#include <vector>

#include "JZAgent.h"
#include "JZAgentTool.h"
#include "JZApplication.h"
#include "JZDebug.h"
#include "JZMcpClient.h"
#include "JZMcpCore.h"
#include "JZMcpServer.h"
#include "JZTimer.h"
#include "json/json.h"

#include "FiveChess.h"
#include "FiveChessTools.h"

using namespace std;

static const char *kDefaultModelUrl  = "http://172.17.160.147:8000/v1/chat/completions";
static const char *kDefaultModelName = "/workspace/RedHatAI/Qwen3.6-35B-A3B-NVFP4";

// ----------------------------------------------------------------------
struct Options
{
    string mode = "single";     // single / dual
    int port = 3900;
    string modelUrl = kDefaultModelUrl;
    string modelA = kDefaultModelName;
    string modelB = kDefaultModelName;
};

// 一个玩家: 要么是大模型 agent, 要么是本地机器人; 两者都通过 MCP 客户端与服务器交互
struct Player
{
    bool isBot = false;
    int color = FC_COLOR_BLACK;
    string name;
    string token;

    JZMcpClient *client = nullptr;
    JZAgent *agent = nullptr;
};

// ----------------------------------------------------------------------
// 对局驱动: 按状态机依次推进 创建房间 -> 加入房间 -> 轮流落子 -> 结束
class GameRunner : public JZObject
{
public:
    enum Stage { S_CREATE, S_JOIN, S_PLAY, S_DONE };

    GameRunner(JZApplication *app, FiveChessHall *hall, const Options &opt);

    bool setup();
    void start();

    int roomId() const { return m_roomId; }
    int moveCount() const { return m_moveCount; }

private:
    JZAgentConfig makeLlmConfig(const string &model) const;

    void requestTurn();
    void botListAndJoin();
    void botPlay();
    void onTurnDone(int color);
    void finish(const string &reason);

    string turnPrompt(int color) const;
    const FiveChessRoom *curRoom() const { return m_hall->room(m_roomId); }

    JZApplication *m_app;
    FiveChessHall *m_hall;
    Options m_opt;
    string m_mcpUrl;

    Player m_players[2];
    Stage m_stage = S_CREATE;
    int m_roomId = 0;
    int m_turn = FC_COLOR_BLACK;
    int m_moveCount = 0;    // 上一次记录的手数, 用于判断这一步是否真的落子
    int m_retry = 0;
    bool m_failed = false;
};

// ----------------------------------------------------------------------
GameRunner::GameRunner(JZApplication *app, FiveChessHall *hall, const Options &opt)
    : JZObject(app), m_app(app), m_hall(hall), m_opt(opt)
{
    m_mcpUrl = "http://127.0.0.1:" + to_string(opt.port) + "/mcp";

    m_players[FC_COLOR_BLACK].color = FC_COLOR_BLACK;
    m_players[FC_COLOR_WHITE].color = FC_COLOR_WHITE;
}

JZAgentConfig GameRunner::makeLlmConfig(const string &model) const
{
    JZAgentConfig cfg;
    cfg.modelUrl = m_opt.modelUrl;
    cfg.modelName = model;
    cfg.modelToken = "EMPTY";           // vLLM 默认不鉴权
    cfg.maxRounds = 6;                  // 一回合内允许的工具调用轮次
    cfg.maxToolError = 5;
    cfg.netTimeout = 180000;
    // 模型支持 160k 上下文, 预算给到 150k, 留 10k 给生成与余量;
    // 真超了仍由 TokenTruncate 裁掉最旧的一半历史兜底。
    cfg.maxToken = 150000;
    cfg.tokenStrategy = JZAgentConfig::TokenTruncate;

    // 服务端只给 512 生成额度, 且默认思考会挤占正文
    Json::Value extra(Json::objectValue);
    extra["max_tokens"] = 512;
    Json::Value tpl(Json::objectValue);
    tpl["enable_thinking"] = false;
    extra["chat_template_kwargs"] = tpl;
    cfg.extraBody = extra;
    return cfg;
}

bool GameRunner::setup()
{
    // ---- 玩家: 黑 ----
    Player &black = m_players[FC_COLOR_BLACK];
    black.isBot = false;
    black.name = "大模型[" + m_opt.modelA + "](黑)";
    // 交给 agent 的 client 必须没有父对象(addMcp 会接管所有权)
    black.client = new JZMcpClient();
    if (!black.client->startHttp(m_mcpUrl))
    {
        jzError() << "黑方连接 MCP 失败:" << black.client->errorString();
        return false;
    }
    black.agent = new JZAgent(this);
    black.agent->init(makeLlmConfig(m_opt.modelA));
    black.agent->addMcp(black.client);
    connect(&black.agent->sigFinished, [this]() { onTurnDone(FC_COLOR_BLACK); });
    connect(&black.agent->sigError, [this](const string &err)
    {
        jzError() << "黑方出错:" << err;
        finish("黑方出错: " + err);
    });

    // ---- 玩家: 白 ----
    Player &white = m_players[FC_COLOR_WHITE];
    // 机器人自己持有 client(不经过 agent); 双 agent 模式下同样要交给 agent, 不能有父对象
    white.client = (m_opt.mode == "single") ? new JZMcpClient(this) : new JZMcpClient();
    if (!white.client->startHttp(m_mcpUrl))
    {
        jzError() << "白方连接 MCP 失败:" << white.client->errorString();
        return false;
    }

    if (m_opt.mode == "single")
    {
        white.isBot = true;
        white.name = "本地机器人(白)";
    }
    else
    {
        white.isBot = false;
        white.name = "大模型[" + m_opt.modelB + "](白)";
        white.agent = new JZAgent(this);
        white.agent->init(makeLlmConfig(m_opt.modelB));
        white.agent->addMcp(white.client);
        connect(&white.agent->sigFinished, [this]() { onTurnDone(FC_COLOR_WHITE); });
        connect(&white.agent->sigError, [this](const string &err)
        {
            jzError() << "白方出错:" << err;
            finish("白方出错: " + err);
        });
    }
    return true;
}

void GameRunner::start()
{
    jzInfo() << "模式:" << m_opt.mode << " | 黑方:" << m_players[0].name
             << " | 白方:" << m_players[1].name;
    m_stage = S_CREATE;
    requestTurn();
}

string GameRunner::turnPrompt(int color) const
{
    const FiveChessRoom *r = curRoom();
    Player p = m_players[color];

    string text = "你在下五子棋(15x15, 五子连珠获胜)。你执" + string(FiveChessHall::colorName(color)) + "。\n";
    if (r && m_stage != S_CREATE)
    {
        text += "room_id = " + to_string(m_roomId) + ", 你的 token = " + p.token + "\n";

        // 只回传最新一手: 对手下的才叫"对手下了什么", 自己下的说明是重试
        if (r->lastColor < 0)
        {
            text += "还没有人落子, 你是先手。\n";
        }
        else if (r->lastColor != color)
        {
            text += "对手(" + string(FiveChessHall::colorName(r->lastColor)) + ")刚刚下在 (x=" +
                    to_string(r->lastX) + ", y=" + to_string(r->lastY) + ")。\n";
        }
        else
        {
            text += "你上一手下在 (x=" + to_string(r->lastX) + ", y=" + to_string(r->lastY) + ")。\n";
        }
    }

    if (m_stage == S_CREATE)
    {
        text += "现在请调用 create_room 工具创建一个房间(房间名任意), 然后把返回的 room_id 和 token 告诉我。";
    }
    else if (m_stage == S_JOIN)
    {
        text += "现在请先用 list_rooms 找到等待中的房间, 再用 join_room 加入它, 然后把你的 token 告诉我。";
    }
    else
    {
        // 棋盘不主动回传, 需要时由模型自己调 get_match 拉取
        text += "轮到你落子: 先调用 get_match(room_id=" + to_string(m_roomId) +
                ") 查看当前棋盘与对局信息, 再调用 play 落子(room_id=" + to_string(m_roomId) +
                ", token=" + p.token + ", x=<列号 0~14>, y=<行号 0~14>)。\n";
        text += "只能下一步, 不要替对手下; 优先连成五子, 同时注意堵住对手的四子。下完用一句话说明思路。";
    }
    return text;
}

void GameRunner::requestTurn()
{
    if (m_stage == S_DONE)
        return;

    Player &p = m_players[m_turn];
    if (p.isBot)
    {
        if (m_stage == S_JOIN)
            botListAndJoin();
        else
            botPlay();
        return;
    }

    jzInfo() << "[" << p.name << "] 请求:" << (m_stage == S_CREATE ? "创建房间" :
                                              (m_stage == S_JOIN ? "加入房间" : "落子"));
    p.agent->request(turnPrompt(m_turn));
}

void GameRunner::botListAndJoin()
{
    Player &bot = m_players[FC_COLOR_WHITE];
    bot.client->callTool("list_rooms", Json::Value(Json::objectValue),
                         [this](bool ok, const Json::Value &)
    {
        if (!ok)
        {
            finish("机器人 list_rooms 失败");
            return;
        }

        Json::Value args(Json::objectValue);
        args["room_id"] = m_roomId;
        m_players[FC_COLOR_WHITE].client->callTool("join_room", args,
                                                   [this](bool ok2, const Json::Value &)
        {
            if (!ok2)
                finish("机器人 join_room 失败");
            else
                onTurnDone(FC_COLOR_WHITE);
        });
    });
}

void GameRunner::botPlay()
{
    const FiveChessRoom *r = curRoom();
    if (!r)
    {
        finish("房间不存在");
        return;
    }

    int x = 0;
    int y = 0;
    if (!fiveChessBotMove(r->board, FC_COLOR_WHITE, x, y))
    {
        finish("机器人无处可下");
        return;
    }

    Json::Value args(Json::objectValue);
    args["room_id"] = m_roomId;
    args["token"] = m_players[FC_COLOR_WHITE].token;
    args["x"] = x;
    args["y"] = y;

    jzInfo() << "[本地机器人(白)] 落子 (x=" << x << ", y=" << y << ")";
    m_players[FC_COLOR_WHITE].client->callTool("play", args,
                                               [this](bool ok, const Json::Value &value)
    {
        if (!ok)
            jzError() << "机器人落子失败:" << value.get("message", "").asString();
        onTurnDone(FC_COLOR_WHITE);
    });
}

void GameRunner::onTurnDone(int color)
{
    if (m_stage == S_DONE)
        return;

    if (m_stage == S_CREATE)
    {
        vector<int> ids = m_hall->roomIds();
        if (ids.empty())
        {
            if (++m_retry > 2)
            {
                finish("黑方没有创建出房间");
                return;
            }
            jzError() << "黑方还没创建房间, 重试(" << m_retry << ")";
            requestTurn();
            return;
        }
        m_retry = 0;
        m_roomId = ids.front();
        const FiveChessRoom *r = curRoom();
        m_players[FC_COLOR_BLACK].token = r->blackToken;
        jzInfo() << "房间已创建: room_id =" << m_roomId << ", 黑方 token =" << r->blackToken;

        m_stage = S_JOIN;
        m_turn = FC_COLOR_WHITE;
        requestTurn();
        return;
    }

    if (m_stage == S_JOIN)
    {
        const FiveChessRoom *r = curRoom();
        if (!r || !r->hasWhite)
        {
            if (++m_retry > 2)
            {
                finish("白方没有加入房间");
                return;
            }
            jzError() << "白方还没加入房间, 重试(" << m_retry << ")";
            requestTurn();
            return;
        }
        m_retry = 0;
        m_players[FC_COLOR_WHITE].token = r->whiteToken;
        jzInfo() << "白方已加入: token =" << r->whiteToken;

        m_stage = S_PLAY;
        m_turn = FC_COLOR_BLACK;      // 黑先
        m_moveCount = 0;
        requestTurn();
        return;
    }

    // ---- 对局中 ----
    const FiveChessRoom *r = curRoom();
    if (!r)
    {
        finish("房间丢失");
        return;
    }

    if (r->result != FR_PLAYING)
    {
        finish(string("对局结束: ") + FiveChessHall::resultName(r->result));
        return;
    }

    if (r->moveCount == m_moveCount)
    {
        // 这一步没有真的落子, 让该方再试一次
        if (++m_retry > 2)
        {
            finish(m_players[color].name + " 连续 3 次没有落子");
            return;
        }
        jzError() << m_players[color].name << " 这一步没有落子, 重试(" << m_retry << ")";
        requestTurn();
        return;
    }

    m_retry = 0;
    m_moveCount = r->moveCount;
    jzInfo() << "第 " << m_moveCount << " 手: " << FiveChessHall::colorName(r->lastColor)
             << " 下在 (x=" << r->lastX << ", y=" << r->lastY << ")";

    m_turn = r->turn;
    requestTurn();
}

void GameRunner::finish(const string &reason)
{
    if (m_stage == S_DONE)
        return;
    m_stage = S_DONE;

    jzInfo() << "================ 对局结束 ================";
    jzInfo() << reason;

    const FiveChessRoom *r = curRoom();
    if (r)
    {
        jzInfo() << "最终棋盘:\n" << r->board.toText();
        jzInfo() << "共 " << r->moveCount << " 手, 结果:" << FiveChessHall::resultName(r->result);
        jzInfo() << "黑方:" << m_players[FC_COLOR_BLACK].name
                 << " | 白方:" << m_players[FC_COLOR_WHITE].name;
    }
    m_app->quit();
}

// ----------------------------------------------------------------------
int main(int argc, char *argv[])
{
    Options opt;
    for (int i = 1; i < argc; i++)
    {
        string a = argv[i];
        if (a == "--mode" && i + 1 < argc)         opt.mode = argv[++i];
        else if (a == "--port" && i + 1 < argc)    opt.port = atoi(argv[++i]);
        else if (a == "--modelUrl" && i + 1 < argc) opt.modelUrl = argv[++i];
        else if (a == "--modelA" && i + 1 < argc)  opt.modelA = argv[++i];
        else if (a == "--modelB" && i + 1 < argc)  opt.modelB = argv[++i];
    }
    if (opt.mode != "single" && opt.mode != "dual")
    {
        jzError() << "未知的 --mode:" << opt.mode << "(可选 single / dual)";
        return 1;
    }

    JZApplication app;

    // ---- MCP 服务器: 五子棋仲裁 ----
    FiveChessHall hall;
    JZMcpServer server;
    server.core()->setServerInfo("FiveChessServer", "1.0.0");
    server.core()->setInstructions("五子棋对弈服务器: 先 create_room 建房间, 对手 list_rooms + join_room 加入, " \
                                   "之后双方用 play 轮流落子, 服务器负责裁定胜负。需要看棋盘时调用 get_match, " \
                                   "服务器不会主动推送棋盘。");
    server.core()->registerTool(new FiveChessCreateRoomTool(&hall));
    server.core()->registerTool(new FiveChessListRoomsTool(&hall));
    server.core()->registerTool(new FiveChessJoinRoomTool(&hall));
    server.core()->registerTool(new FiveChessGetMatchTool(&hall));
    server.core()->registerTool(new FiveChessPlayTool(&hall));

    if (!server.listenHttp(opt.port))
    {
        jzError() << "MCP 服务启动失败, port =" << opt.port;
        return 1;
    }
    jzInfo() << "五子棋 MCP 服务已启动: http://127.0.0.1:" << opt.port << "/mcp";

    // ---- 对局驱动 ----
    GameRunner runner(&app, &hall, opt);
    if (!runner.setup())
        return 1;
    runner.start();

    // 兜底超时: 一整盘棋 + 大模型思考, 给足时间
    JZTimer::singleShot(900000, [&app]()
    {
        jzError() << "整局超时";
        app.quit();
    });

    app.exec();

    const FiveChessRoom *r = hall.room(runner.roomId());
    if (!r || r->result == FR_PLAYING)
    {
        jzInfo() << "对局未分出胜负(共 " << runner.moveCount() << " 手)";
        return 1;
    }
    jzInfo() << "测试完成, 结果:" << FiveChessHall::resultName(r->result);
    return 0;
}
