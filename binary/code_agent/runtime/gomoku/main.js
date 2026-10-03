// 五子棋 (Gomoku) — 使用 JZJSWidgetModule 注册的 widget 类
//   JZWindow / JZWidget(画布) / JZLabel / JZPushButton / JZRect / JZPoint / JZPainter
// 运行: code_runtime --entry <本目录>/main.js
//
// 注意: win / canvas / status / restart 必须处于模块顶层作用域 (ESM 模块对象会被引擎保留),
// 不能被 GC 回收, 否则会销毁底层窗口。
//
// 2026-09-18 更新: 旧的 `win.createCanvas(w, h)`(JZCanvas) 包装已从引擎移除 ——
//   画布 = **普通 JZWidget + onPaint + JZPainter**(与 test_jswidget/test_jsshooter 同款)。
//
// 2026-10-02 更新: 加入电脑对手(执白), 逻辑移植自 test/agent/five_chess/FiveChess.cpp
//   的 fiveChessBotMove(能赢就赢/能堵就堵, 只看已有棋子附近 2 格的空位)。
//   玩家执黑, 总是先走; 玩家落子后电脑立刻回一手。

const GRID = 15;            // 15x15
const CELL = 40;            // 格距(逻辑像素)
const MARGIN = 20;          // 棋盘四周留白
const STATUS_H = 36;        // 顶部状态栏高度
const BTN_H = 40;           // 底部按钮高度
const BOARD = MARGIN * 2 + (GRID - 1) * CELL;
const WIN_W = BOARD;
const WIN_H = STATUS_H + BOARD + BTN_H;

const EMPTY = 0, BLACK = 1, WHITE = 2;
const HUMAN = BLACK;        // 玩家执黑, 总是先走
const AI = WHITE;           // 电脑执白

const board = [];
for (let i = 0; i < GRID; i++) board.push(new Array(GRID).fill(EMPTY));
let turn = HUMAN;           // 玩家先走
let winner = EMPTY;
let over = false;
let moveNo = 0;             // 已落子手数

// 2026-10-02: 打日志 —— 外面(runs/current 的证据 / 玩法的尺子 verify.py)靠这些行判这局的结果。
//   标记**一律 ASCII**(容器转发程序 stdout 时中文可能坏码; 判定只认 ASCII)。
function log(s) { console.log("[gomoku] " + s); }

/** 整盘棋的文本快照(日志里看一眼就知道发生了什么) */
function boardDump()
{
    for (let r = 0; r < GRID; r++)
    {
        let s = "";
        for (let c = 0; c < GRID; c++)
            s += (board[r][c] === EMPTY ? "." : (board[r][c] === BLACK ? "X" : "O"));
        log("board " + (r < 10 ? "0" : "") + r + " " + s);
    }
}

const win = new JZWindow(WIN_W, WIN_H, "五子棋");

// 窗口的 addWidget 会按需补一个纵向布局(VBox, margin/spacing = 0), 所以三个子控件
// 各自给固定尺寸, 从上到下就是: 状态栏 / 棋盘 / 按钮。
// 顶部状态栏 (JZLabel)
const status = new JZLabel();
status.setText("轮到你 (黑棋, 点击落子)");
status.setAlign(1);         // 1 = 居中
status.setFixedSize(BOARD, STATUS_H);
win.addWidget(status);

// 棋盘画布: 普通 JZWidget + onPaint 里用 JZPainter 画
const canvas = new JZWidget();
canvas.setFixedSize(BOARD, BOARD);
win.addWidget(canvas);

// 底部重新开局按钮 (JZPushButton)
const restart = new JZPushButton();
restart.setText("重新开始");
restart.setFixedSize(BOARD, BTN_H);
win.addWidget(restart);

// 用几何类做命中测试
const boardRect = new JZRect(MARGIN, MARGIN, (GRID - 1) * CELL, (GRID - 1) * CELL);

function statusText()
{
    if (over)  return winner === HUMAN ? "你赢了!" : (winner === AI ? "电脑赢了!" : "平局");
    return turn === HUMAN ? "轮到你 (黑棋, 点击落子)" : "电脑思考中...";
}

function draw(ctx)
{
    // 背景
    ctx.fillRect(0, 0, BOARD, BOARD, "#e3b572");

    // 棋盘网格
    ctx.setPen("#3a2a12", 1);
    for (let i = 0; i < GRID; i++)
    {
        const g = MARGIN + i * CELL;
        ctx.drawLine(MARGIN, g, MARGIN + (GRID - 1) * CELL, g);
        ctx.drawLine(g, MARGIN, g, MARGIN + (GRID - 1) * CELL);
    }

    // 星位
    const stars = [[3,3],[3,7],[3,11],[7,3],[7,7],[7,11],[11,3],[11,7],[11,11]];
    for (const [sr, sc] of stars)
        ctx.fillEllipse(MARGIN + sc * CELL - 3, MARGIN + sr * CELL - 3, 6, 6, "#3a2a12");

    // 棋子
    const STONE_R = 17;
    for (let r = 0; r < GRID; r++)
    {
        for (let c = 0; c < GRID; c++)
        {
            const v = board[r][c];
            if (v === EMPTY) continue;
            const cx = MARGIN + c * CELL, cy = MARGIN + r * CELL;
            ctx.fillEllipse(cx - STONE_R, cy - STONE_R, STONE_R * 2, STONE_R * 2,
                            v === BLACK ? "#111111" : "#f6f6f6");
            ctx.setPen("#3a3a3a", 1);
            ctx.drawEllipse(cx - STONE_R, cy - STONE_R, STONE_R * 2, STONE_R * 2);
        }
    }
}

function checkWin(r, c, who)
{
    const dirs = [[1,0],[0,1],[1,1],[1,-1]];
    for (const [dr, dc] of dirs)
    {
        let cnt = 1;
        for (let s = 1; s < 5; s++) { const rr = r+dr*s, cc = c+dc*s; if (rr<0||cc<0||rr>=GRID||cc>=GRID||board[rr][cc]!==who) break; cnt++; }
        for (let s = 1; s < 5; s++) { const rr = r-dr*s, cc = c-dc*s; if (rr<0||cc<0||rr>=GRID||cc>=GRID||board[rr][cc]!==who) break; cnt++; }
        if (cnt >= 5) return true;
    }
    return false;
}

// 落子并判定胜负/平局(不负责切换 turn, 由调用方决定)
function place(r, c, who)
{
    board[r][c] = who;
    moveNo++;
    log((who === HUMAN ? "human" : "ai") + " move r=" + r + " c=" + c + " no=" + moveNo);
    if (checkWin(r, c, who))
    {
        winner = who;
        over = true;
        log("RESULT=" + (who === HUMAN ? "human_win" : "ai_win")
            + " winner=" + (who === HUMAN ? "human" : "ai") + " moves=" + moveNo);
        boardDump();
        return;
    }
    // 平局判定: 棋盘满
    for (let i = 0; i < GRID; i++)
        for (let j = 0; j < GRID; j++)
            if (board[i][j] === EMPTY)
                return;
    over = true;
    log("RESULT=draw over=1 moves=" + moveNo);
    boardDump();
}

// ==================== 电脑对手(白棋) ====================
// 某空位落子后四个方向能连成几子(只算过该点的方向), 同 C++ 版 moveScore
function moveScore(r0, c0, who)
{
    const dirs = [[1, 0], [0, 1], [1, 1], [1, -1]];
    let best = 0;
    for (const [dr, dc] of dirs)
    {
        let count = 1, openEnds = 0;
        for (let s = -1; s <= 1; s += 2)
        {
            let rr = r0 + dr * s, cc = c0 + dc * s;
            while (rr >= 0 && cc >= 0 && rr < GRID && cc < GRID && board[rr][cc] === who)
            {
                count++;
                rr += dr * s;
                cc += dc * s;
            }
            if (rr >= 0 && cc >= 0 && rr < GRID && cc < GRID && board[rr][cc] === EMPTY)
                openEnds++;
        }

        let score = count * 10 + openEnds;
        if (count >= 5)
            score = 100000;
        else if (count === 4 && openEnds >= 1)
            score = 10000;
        if (score > best)
            best = score;
    }
    return best;
}

// 该空位附近 2 格内是否有棋子(避免全局乱下)
function nearPiece(r0, c0)
{
    for (let dr = -2; dr <= 2; dr++)
        for (let dc = -2; dc <= 2; dc++)
        {
            const rr = r0 + dr, cc = c0 + dc;
            if (rr >= 0 && cc >= 0 && rr < GRID && cc < GRID && board[rr][cc] !== EMPTY)
                return true;
        }
    return false;
}

function aiMove()
{
    let bestR = -1, bestC = -1, bestScore = -1;
    for (let r = 0; r < GRID; r++)
    {
        for (let c = 0; c < GRID; c++)
        {
            if (board[r][c] !== EMPTY || !nearPiece(r, c)) continue;
            const score = moveScore(r, c, AI) + moveScore(r, c, HUMAN);   // 兼顾进攻与堵截
            if (score > bestScore)
            {
                bestScore = score;
                bestR = r;
                bestC = c;
            }
        }
    }

    if (bestR < 0)          // 空棋盘: 天元开局(玩家先走, 正常走不到这里)
    {
        const mid = (GRID - 1) / 2;
        if (board[mid][mid] !== EMPTY) return;
        bestR = mid;
        bestC = mid;
    }

    place(bestR, bestC, AI);
}

function reset()
{
    for (let i = 0; i < GRID; i++)
        for (let j = 0; j < GRID; j++)
            board[i][j] = EMPTY;
    turn = HUMAN;
    winner = EMPTY;
    over = false;
    moveNo = 0;
    log("restart");
    status.setText(statusText());
    canvas.update();
}

canvas.onPaint = (e) => {
    const ctx = new JZPainter(canvas);   // 与 C++ 一样: 在绘制回调里自建画笔(本次绘制内有效)
    draw(ctx);
};

canvas.onMousePress = (e) => {
    const x = e.localX();
    const y = e.localY();
    const button = e.button();
    if (over || button !== 1) return;            // 仅左键(JZMouseEvent: 1=Left 2=Right 3=Middle)
    if (turn !== HUMAN) return;                  // 只有轮到玩家时才响应点击
    if (!boardRect.contains(x, y)) return;       // 用 JZRect 做边界判定
    const pt = new JZPoint(x, y);                // 用 JZPoint 取坐标
    const c = Math.round((pt.x() - MARGIN) / CELL);
    const r = Math.round((pt.y() - MARGIN) / CELL);
    if (c < 0 || c >= GRID || r < 0 || r >= GRID) return;
    if (board[r][c] !== EMPTY) return;

    // 玩家一手
    place(r, c, HUMAN);

    // 电脑立刻回一手(同步, 不用定时器)
    if (!over)
    {
        turn = AI;
        status.setText("电脑思考中...");
        canvas.update();
        aiMove();
        turn = HUMAN;
    }

    status.setText(statusText());
    canvas.update();
};   // <- 这里是"箭头函数赋值"的收尾(原文件写成 `});`, 多一个右括号 = 语法错, 这才是它起不来的直接原因)

restart.onClicked = () => reset();   // 信号是属性

win.show();
log("start grid=" + GRID + " cell=" + CELL + " margin=" + MARGIN + " statusH=" + STATUS_H
    + " board=" + BOARD + "x" + BOARD + " win=" + WIN_W + "x" + WIN_H + " human=black ai=white");
