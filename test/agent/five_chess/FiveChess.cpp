#include "FiveChess.h"

#include <cstdlib>

// ==================== FiveChessBoard ====================
FiveChessBoard::FiveChessBoard()
{
    clear();
}

void FiveChessBoard::clear()
{
    for (int y = 0; y < SIZE; y++)
        for (int x = 0; x < SIZE; x++)
            m_cells[y][x] = FC_EMPTY;
}

bool FiveChessBoard::inBoard(int x, int y) const
{
    return x >= 0 && x < SIZE && y >= 0 && y < SIZE;
}

FiveCell FiveChessBoard::at(int x, int y) const
{
    if (!inBoard(x, y))
        return FC_EMPTY;
    return m_cells[y][x];
}

bool FiveChessBoard::isEmpty(int x, int y) const
{
    return inBoard(x, y) && m_cells[y][x] == FC_EMPTY;
}

void FiveChessBoard::set(int x, int y, FiveCell c)
{
    if (inBoard(x, y))
        m_cells[y][x] = c;
}

bool FiveChessBoard::full() const
{
    for (int y = 0; y < SIZE; y++)
        for (int x = 0; x < SIZE; x++)
            if (m_cells[y][x] == FC_EMPTY)
                return false;
    return true;
}

// 四个方向: 横 / 竖 / 撇 / 捺, 双向计数, >= 5 即胜
bool FiveChessBoard::isFive(int x, int y) const
{
    static const int dirs[4][2] = { {1, 0}, {0, 1}, {1, 1}, {1, -1} };

    FiveCell c = at(x, y);
    if (c == FC_EMPTY)
        return false;

    for (int d = 0; d < 4; d++)
    {
        int count = 1;
        for (int s = -1; s <= 1; s += 2)
        {
            int dx = dirs[d][0] * s;
            int dy = dirs[d][1] * s;
            int cx = x + dx;
            int cy = y + dy;
            while (inBoard(cx, cy) && at(cx, cy) == c)
            {
                count++;
                cx += dx;
                cy += dy;
            }
        }
        if (count >= 5)
            return true;
    }
    return false;
}

std::string FiveChessBoard::toText() const
{
    // 列号 0-14, 用十六进制一位表示(0..9,a..e), 方便模型对齐
    static const char *cols = "0123456789abcde";

    std::string text = "   " + std::string(cols) + "\n";
    for (int y = 0; y < SIZE; y++)
    {
        std::string line;
        if (y < 10)
            line += " ";
        line += std::to_string(y) + " ";
        for (int x = 0; x < SIZE; x++)
        {
            char c = '.';
            if (m_cells[y][x] == FC_BLACK)
                c = 'X';
            else if (m_cells[y][x] == FC_WHITE)
                c = 'O';
            line += c;
        }
        text += line + "\n";
    }
    return text;
}

// ==================== FiveChessHall ====================
const char *FiveChessHall::colorName(int color)
{
    return color == FC_COLOR_BLACK ? "黑棋(X)" : "白棋(O)";
}

const char *FiveChessHall::resultName(FiveResult r)
{
    switch (r)
    {
    case FR_BLACK_WIN: return "黑棋胜";
    case FR_WHITE_WIN: return "白棋胜";
    case FR_DRAW:      return "平局";
    default:           return "对局进行中";
    }
}

std::string FiveChessHall::makeToken()
{
    // 简单递增 + 两位随机数, 够用来区分玩家且不容易被模型猜到
    int r = 10 + (rand() % 90);
    return "tk" + std::to_string(m_nextToken++) + "-" + std::to_string(r);
}

int FiveChessHall::createRoom(const std::string &name, std::string &token, std::string &error)
{
    FiveChessRoom room;
    room.id = m_nextId++;
    room.name = name.empty() ? ("room" + std::to_string(room.id)) : name;
    room.blackToken = makeToken();
    m_rooms.push_back(room);

    token = room.blackToken;
    return room.id;
}

bool FiveChessHall::joinRoom(int roomId, std::string &token, std::string &error)
{
    for (auto &r : m_rooms)
    {
        if (r.id != roomId)
            continue;
        if (r.hasWhite)
        {
            error = "房间已满, 无法加入: " + std::to_string(roomId);
            return false;
        }
        r.hasWhite = true;
        r.whiteToken = makeToken();
        token = r.whiteToken;
        return true;
    }

    error = "房间不存在: " + std::to_string(roomId);
    return false;
}

std::vector<int> FiveChessHall::roomIds() const
{
    std::vector<int> ids;
    for (const auto &r : m_rooms)
        ids.push_back(r.id);
    return ids;
}

std::vector<int> FiveChessHall::waitingRoomIds() const
{
    std::vector<int> ids;
    for (const auto &r : m_rooms)
    {
        if (!r.hasWhite)
            ids.push_back(r.id);
    }
    return ids;
}

const FiveChessRoom *FiveChessHall::room(int roomId) const
{
    for (const auto &r : m_rooms)
    {
        if (r.id == roomId)
            return &r;
    }
    return nullptr;
}

bool FiveChessHall::findByToken(const std::string &token, int &roomId, int &color) const
{
    for (const auto &r : m_rooms)
    {
        if (!r.blackToken.empty() && r.blackToken == token)
        {
            roomId = r.id;
            color = FC_COLOR_BLACK;
            return true;
        }
        if (r.hasWhite && !r.whiteToken.empty() && r.whiteToken == token)
        {
            roomId = r.id;
            color = FC_COLOR_WHITE;
            return true;
        }
    }
    return false;
}

bool FiveChessHall::play(const std::string &token, int x, int y, std::string &error)
{
    int room_id = 0;
    int color = 0;
    if (!findByToken(token, room_id, color))
    {
        error = "token 无效: " + token;
        return false;
    }

    for (auto &r : m_rooms)
    {
        if (r.id != room_id)
            continue;

        if (r.result != FR_PLAYING)
        {
            error = "对局已结束(" + std::string(resultName(r.result)) + "), 不能再落子";
            return false;
        }
        if (!r.hasWhite)
        {
            error = "对手还未加入房间, 请稍候";
            return false;
        }
        if (r.turn != color)
        {
            error = "还没轮到你, 当前该" + std::string(colorName(r.turn)) + "走";
            return false;
        }
        if (!r.board.inBoard(x, y))
        {
            error = "坐标越界, x 与 y 都必须是 0~14";
            return false;
        }
        if (!r.board.isEmpty(x, y))
        {
            error = "该位置已经有棋子了";
            return false;
        }

        r.board.set(x, y, color == FC_COLOR_BLACK ? FC_BLACK : FC_WHITE);
        r.moveCount++;
        r.lastX = x;
        r.lastY = y;
        r.lastColor = color;

        if (r.board.isFive(x, y))
            r.result = (color == FC_COLOR_BLACK) ? FR_BLACK_WIN : FR_WHITE_WIN;
        else if (r.board.full())
            r.result = FR_DRAW;
        else
            r.turn = (color == FC_COLOR_BLACK) ? FC_COLOR_WHITE : FC_COLOR_BLACK;

        return true;
    }

    error = "房间不存在: " + std::to_string(room_id);
    return false;
}

std::string FiveChessHall::matchInfo(int roomId, std::string &error) const
{
    const FiveChessRoom *r = room(roomId);
    if (!r)
    {
        error = "房间不存在: " + std::to_string(roomId);
        return std::string();
    }

    std::string text = "房间 " + std::to_string(r->id) + " (" + r->name + ")\n";
    text += "棋盘(X 黑棋, O 白棋, . 空位; 首行是列号, 首列是行号, 坐标范围 0~14):\n";
    text += r->board.toText();
    text += "已下 " + std::to_string(r->moveCount) + " 手\n";
    text += lastMoveText();
    if (r->result == FR_PLAYING)
        text += std::string("轮到") + colorName(r->turn);
    else
        text += std::string("结果: ") + resultName(r->result);
    return text;
}

std::string FiveChessHall::lastMoveText() const
{
    // 取最近一手的房间: 手数最多的那个(这里只有一个对局房间)
    const FiveChessRoom *best = nullptr;
    for (const auto &r : m_rooms)
    {
        if (r.lastColor < 0)
            continue;
        if (!best || r.moveCount > best->moveCount)
            best = &r;
    }
    if (!best)
        return "还没有人落子\n";

    return std::string("最近一手: ") + colorName(best->lastColor) +
           " 下在 (x=" + std::to_string(best->lastX) + ", y=" + std::to_string(best->lastY) + ")\n";
}

// ==================== 本地机器人 ====================
// 给某个空位打分: 该点落子后能连成几子(只算过该点的四个方向)
static int moveScore(const FiveChessBoard &board, int x, int y, int color)
{
    static const int dirs[4][2] = { {1, 0}, {0, 1}, {1, 1}, {1, -1} };
    FiveCell me = (color == FC_COLOR_BLACK) ? FC_BLACK : FC_WHITE;

    int best = 0;
    for (int d = 0; d < 4; d++)
    {
        int count = 1;
        int open_ends = 0;
        for (int s = -1; s <= 1; s += 2)
        {
            int dx = dirs[d][0] * s;
            int dy = dirs[d][1] * s;
            int cx = x + dx;
            int cy = y + dy;
            while (board.inBoard(cx, cy) && board.at(cx, cy) == me)
            {
                count++;
                cx += dx;
                cy += dy;
            }
            if (board.inBoard(cx, cy) && board.at(cx, cy) == FC_EMPTY)
                open_ends++;
        }

        int score = count * 10 + open_ends;
        if (count >= 5)
            score = 100000;
        else if (count == 4 && open_ends >= 1)
            score = 10000;
        if (score > best)
            best = score;
    }
    return best;
}

bool fiveChessBotMove(const FiveChessBoard &board, int color, int &x, int &y)
{
    int enemy = (color == FC_COLOR_BLACK) ? FC_COLOR_WHITE : FC_COLOR_BLACK;

    // 只考虑已有棋子附近 2 格内的空位, 避免全局乱下
    auto nearPiece = [&board](int px, int py)
    {
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++)
            {
                int cx = px + dx;
                int cy = py + dy;
                if (board.inBoard(cx, cy) && board.at(cx, cy) != FC_EMPTY)
                    return true;
            }
        return false;
    };

    int best_x = -1;
    int best_y = -1;
    int best_score = -1;
    for (int py = 0; py < FiveChessBoard::SIZE; py++)
    {
        for (int px = 0; px < FiveChessBoard::SIZE; px++)
        {
            if (!board.isEmpty(px, py))
                continue;
            if (!nearPiece(px, py))
                continue;

            int attack = moveScore(board, px, py, color);
            int defend = moveScore(board, px, py, enemy);
            int score = attack + defend;      // 兼顾进攻与堵截
            if (score > best_score)
            {
                best_score = score;
                best_x = px;
                best_y = py;
            }
        }
    }

    // 空棋盘: 天元开局
    if (best_x < 0)
    {
        int mid = FiveChessBoard::SIZE / 2;
        if (!board.isEmpty(mid, mid))
            return false;
        x = mid;
        y = mid;
        return true;
    }

    x = best_x;
    y = best_y;
    return true;
}
