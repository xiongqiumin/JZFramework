#include "FiveChessTools.h"

#include "JZMcpCore.h"

// MCP 结果对象: { content:[{type:"text",text:...}], isError:bool }
static Json::Value fcTextResult(const std::string &text, bool isError = false)
{
    Json::Value result(Json::objectValue);
    Json::Value content(Json::arrayValue);
    Json::Value item(Json::objectValue);
    item["type"] = "text";
    item["text"] = text;
    content.append(item);
    result["content"] = content;
    result["isError"] = isError;
    return result;
}

static Json::Value fcNumberSchema(const std::string &desc)
{
    Json::Value schema(Json::objectValue);
    schema["type"] = "object";
    Json::Value props(Json::objectValue);
    Json::Value v(Json::objectValue);
    v["type"] = "number";
    props["value"] = v;
    schema["properties"] = props;
    Json::Value required(Json::arrayValue);
    required.append("value");
    schema["required"] = required;
    return schema;
}

// ==================== create_room ====================
FiveChessCreateRoomTool::FiveChessCreateRoomTool(FiveChessHall *hall)
    : JZMcpTool(nullptr), m_hall(hall)
{
    setName("create_room");
    setDescription("创建一个五子棋房间。创建者执黑棋(X)并先手。返回 room_id 和你的 token, " \
                   "token 是你在该房间的身份, 之后每次 play 都要带上它。");

    Json::Value schema(Json::objectValue);
    schema["type"] = "object";
    Json::Value props(Json::objectValue);
    Json::Value name(Json::objectValue);
    name["type"] = "string";
    props["name"] = name;
    schema["properties"] = props;
    setInputSchema(schema);
}

void FiveChessCreateRoomTool::run(const Json::Value &id, const Json::Value &args)
{
    std::string error;
    std::string token;
    int room_id = m_hall->createRoom(args.get("name", "").asString(), token, error);
    if (room_id <= 0)
    {
        sigFinished.emit(id, fcTextResult("创建房间失败: " + error, true));
        return;
    }

    std::string text = "房间已创建: room_id = " + std::to_string(room_id) + "\n";
    text += "你执黑棋(X), 先手。你的 token = " + token + "\n";
    text += "等待白棋加入后才能落子, 期间可以调用 get_board 查看房间状态。";
    sigFinished.emit(id, fcTextResult(text));
}

// ==================== list_rooms ====================
FiveChessListRoomsTool::FiveChessListRoomsTool(FiveChessHall *hall)
    : JZMcpTool(nullptr), m_hall(hall)
{
    setName("list_rooms");
    setDescription("列出所有还在等待对手加入的五子棋房间(返回 room_id 与房间名)。");
    setInputSchema(Json::Value(Json::objectValue));
}

void FiveChessListRoomsTool::run(const Json::Value &id, const Json::Value &args)
{
    std::vector<int> ids = m_hall->waitingRoomIds();
    std::string text;
    if (ids.empty())
    {
        text = "当前没有等待中的房间, 可以用 create_room 新建一个。";
    }
    else
    {
        text = "等待加入的房间:\n";
        for (int id_ : ids)
        {
            const FiveChessRoom *r = m_hall->room(id_);
            text += "  room_id = " + std::to_string(id_) +
                    (r ? ("  名称: " + r->name) : std::string()) + "\n";
        }
        text += "用 join_room 加入其中一个, 加入后你执白棋(O)。";
    }
    sigFinished.emit(id, fcTextResult(text));
}

// ==================== join_room ====================
FiveChessJoinRoomTool::FiveChessJoinRoomTool(FiveChessHall *hall)
    : JZMcpTool(nullptr), m_hall(hall)
{
    setName("join_room");
    setDescription("加入一个五子棋房间, 加入者执白棋(O), 后手。返回你的 token。");

    Json::Value schema(Json::objectValue);
    schema["type"] = "object";
    Json::Value props(Json::objectValue);
    Json::Value rid(Json::objectValue);
    rid["type"] = "number";
    props["room_id"] = rid;
    schema["properties"] = props;
    Json::Value required(Json::arrayValue);
    required.append("room_id");
    schema["required"] = required;
    setInputSchema(schema);
}

void FiveChessJoinRoomTool::run(const Json::Value &id, const Json::Value &args)
{
    int room_id = args.get("room_id", 0).asInt();

    std::string error;
    std::string token;
    if (!m_hall->joinRoom(room_id, token, error))
    {
        sigFinished.emit(id, fcTextResult("加入房间失败: " + error, true));
        return;
    }

    std::string text = "已加入房间 " + std::to_string(room_id) + "\n";
    text += "你执白棋(O), 后手。你的 token = " + token + "\n";
    text += "黑棋先走, 轮到你时再调用 play 落子。";
    sigFinished.emit(id, fcTextResult(text));
}

// ==================== get_match ====================
FiveChessGetMatchTool::FiveChessGetMatchTool(FiveChessHall *hall)
    : JZMcpTool(nullptr), m_hall(hall)
{
    setName("get_match");
    setDescription("获取对局信息: 15x15 棋盘(X 黑棋, O 白棋, . 空位)、已下手数、最近一手、" \
                   "当前轮到谁、是否已分出胜负。列号见棋盘首行, 行号见首列, 坐标范围都是 0~14。" \
                   "每次要落子前都应该先调用它确认盘面。");

    Json::Value schema(Json::objectValue);
    schema["type"] = "object";
    Json::Value props(Json::objectValue);
    Json::Value rid(Json::objectValue);
    rid["type"] = "number";
    props["room_id"] = rid;
    schema["properties"] = props;
    Json::Value required(Json::arrayValue);
    required.append("room_id");
    schema["required"] = required;
    setInputSchema(schema);
}

void FiveChessGetMatchTool::run(const Json::Value &id, const Json::Value &args)
{
    int room_id = args.get("room_id", 0).asInt();

    std::string error;
    std::string text = m_hall->matchInfo(room_id, error);
    if (!error.empty())
    {
        sigFinished.emit(id, fcTextResult(error, true));
        return;
    }
    sigFinished.emit(id, fcTextResult(text));
}

// ==================== play ====================
FiveChessPlayTool::FiveChessPlayTool(FiveChessHall *hall)
    : JZMcpTool(nullptr), m_hall(hall)
{
    setName("play");
    setDescription("在房间里落子。参数: room_id(房间号)、token(你的身份)、x/y(坐标, 0~14, " \
                   "x 是列号, y 是行号)。只有轮到你时才能落子, 位置必须为空。返回落子后的棋盘与结果。");

    Json::Value schema(Json::objectValue);
    schema["type"] = "object";
    Json::Value props(Json::objectValue);
    Json::Value rid(Json::objectValue);
    rid["type"] = "number";
    Json::Value token(Json::objectValue);
    token["type"] = "string";
    Json::Value x(Json::objectValue);
    x["type"] = "number";
    Json::Value y(Json::objectValue);
    y["type"] = "number";
    props["room_id"] = rid;
    props["token"] = token;
    props["x"] = x;
    props["y"] = y;
    schema["properties"] = props;

    Json::Value required(Json::arrayValue);
    required.append("room_id");
    required.append("token");
    required.append("x");
    required.append("y");
    schema["required"] = required;
    setInputSchema(schema);
}

void FiveChessPlayTool::run(const Json::Value &id, const Json::Value &args)
{
    std::string token = args.get("token", "").asString();
    int x = args.get("x", -1).asInt();
    int y = args.get("y", -1).asInt();
    args.get("room_id", 0).asInt();   // room_id 只用于日志提示, 落子以 token 为准

    std::string error;
    if (!m_hall->play(token, x, y, error))
    {
        // 非法落子: 只回原因, 让模型自己调 get_match 重新看盘(避免每次都回传整张棋盘)
        sigFinished.emit(id, fcTextResult("落子失败: " + error +
                                          "。请用 get_match 查看当前棋盘后重新选择空位。", true));
        return;
    }

    int room_id = 0;
    int color = 0;
    m_hall->findByToken(token, room_id, color);

    // 成功也只回结果与状态, 不带棋盘
    std::string text = "落子成功: " + std::string(FiveChessHall::colorName(color)) +
                       " 下在 (x=" + std::to_string(x) + ", y=" + std::to_string(y) + ")\n";

    const FiveChessRoom *r = m_hall->room(room_id);
    if (r && r->result != FR_PLAYING)
        text += "对局结束: " + std::string(FiveChessHall::resultName(r->result));
    else if (r)
        text += std::string("已下 ") + std::to_string(r->moveCount) + " 手, 轮到" +
                FiveChessHall::colorName(r->turn);

    sigFinished.emit(id, fcTextResult(text));
}
