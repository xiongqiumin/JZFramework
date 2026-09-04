#ifndef FIVE_CHESS_TOOLS_H_
#define FIVE_CHESS_TOOLS_H_

#include "JZMcpTool.h"
#include "FiveChess.h"

// ----------------------------------------------------------------------
// 五子棋 MCP 工具集: 房间由大厅(FiveChessHall)仲裁, 工具只做参数校验与结果拼装。
//
//   create_room  创建房间, 创建者执黑, 返回 room_id 与自己的 token
//   list_rooms   列出等待对手加入的房间
//   join_room    加入房间, 加入者执白, 返回自己的 token
//   get_match    主动拉取对局信息(棋盘 + 已下手数 + 最近一手 + 轮到谁/结果)
//   play         落子(token + 坐标), 由大厅裁决胜负
//
// 棋盘不随每一轮提示主动回传, 模型需要看盘时自己调 get_match;
// play 的返回里也只带落子结果与当前状态, 不带棋盘。
// ----------------------------------------------------------------------
class FiveChessCreateRoomTool : public JZMcpTool
{
public:
    explicit FiveChessCreateRoomTool(FiveChessHall *hall);
    void run(const Json::Value &id, const Json::Value &args) override;

private:
    FiveChessHall *m_hall;
};

class FiveChessListRoomsTool : public JZMcpTool
{
public:
    explicit FiveChessListRoomsTool(FiveChessHall *hall);
    void run(const Json::Value &id, const Json::Value &args) override;

private:
    FiveChessHall *m_hall;
};

class FiveChessJoinRoomTool : public JZMcpTool
{
public:
    explicit FiveChessJoinRoomTool(FiveChessHall *hall);
    void run(const Json::Value &id, const Json::Value &args) override;

private:
    FiveChessHall *m_hall;
};

class FiveChessGetMatchTool : public JZMcpTool
{
public:
    explicit FiveChessGetMatchTool(FiveChessHall *hall);
    void run(const Json::Value &id, const Json::Value &args) override;

private:
    FiveChessHall *m_hall;
};

class FiveChessPlayTool : public JZMcpTool
{
public:
    explicit FiveChessPlayTool(FiveChessHall *hall);
    void run(const Json::Value &id, const Json::Value &args) override;

private:
    FiveChessHall *m_hall;
};

#endif // !FIVE_CHESS_TOOLS_H_
