#ifndef FIVE_CHESS_H_
#define FIVE_CHESS_H_

#include <string>
#include <vector>

// ----------------------------------------------------------------------
// 棋盘 / 规则 / 房间仲裁(纯逻辑, 不依赖任何框架)
// ----------------------------------------------------------------------
enum FiveCell { FC_EMPTY = 0, FC_BLACK = 1, FC_WHITE = 2 };
enum FiveResult { FR_PLAYING = 0, FR_BLACK_WIN = 1, FR_WHITE_WIN = 2, FR_DRAW = 3 };

// 颜色: 0 = 黑(先手), 1 = 白
enum FiveColor { FC_COLOR_BLACK = 0, FC_COLOR_WHITE = 1 };

class FiveChessBoard
{
public:
    static const int SIZE = 15;

    FiveChessBoard();

    void clear();
    bool inBoard(int x, int y) const;
    FiveCell at(int x, int y) const;
    bool isEmpty(int x, int y) const;
    void set(int x, int y, FiveCell c);
    bool full() const;

    // 以 (x,y) 为端点检查是否形成五连
    bool isFive(int x, int y) const;

    // 15 行文本: '.' 空, 'X' 黑, 'O' 白, 带行号与列号
    std::string toText() const;

private:
    FiveCell m_cells[SIZE][SIZE];
};

struct FiveChessRoom
{
    int id = 0;
    std::string name;
    std::string blackToken;
    std::string whiteToken;
    bool hasWhite = false;

    FiveChessBoard board;
    int turn = FC_COLOR_BLACK;      // 当前该谁走
    FiveResult result = FR_PLAYING;
    int moveCount = 0;
    int lastX = -1;
    int lastY = -1;
    int lastColor = -1;
};

// 仲裁中心: 房间管理 + 落子合法性 + 胜负判定
class FiveChessHall
{
public:
    // 创建房间, 创建者执黑, 返回房间 id 与黑方 token
    int createRoom(const std::string &name, std::string &token, std::string &error);
    // 加入房间, 加入者执白, 返回白方 token
    bool joinRoom(int roomId, std::string &token, std::string &error);

    std::vector<int> roomIds() const;
    std::vector<int> waitingRoomIds() const;   // 还缺白棋的房间
    const FiveChessRoom *room(int roomId) const;

    // token -> (房间, 颜色)
    bool findByToken(const std::string &token, int &roomId, int &color) const;

    // 落子: 校验身份/回合/坐标, 落子后更新胜负状态
    bool play(const std::string &token, int x, int y, std::string &error);

    // 完整对局信息(棋盘 + 已下手数 + 最近一手 + 轮到谁/结果), 只在模型主动调用 get_match 时返回
    std::string matchInfo(int roomId, std::string &error) const;

    // 最近一手的简短描述(给玩家看对手下了什么), 没有人落子时返回空
    std::string lastMoveText() const;

    static const char *colorName(int color);
    static const char *resultName(FiveResult r);

private:
    std::string makeToken();

    std::vector<FiveChessRoom> m_rooms;
    int m_nextId = 1;
    int m_nextToken = 1;
};

// 本地机器人(单 agent 模式下的人类替身): 能赢就赢, 能堵就堵, 否则贴着已有的棋子下
bool fiveChessBotMove(const FiveChessBoard &board, int color, int &x, int &y);

#endif // !FIVE_CHESS_H_
