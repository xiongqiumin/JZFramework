// 五子棋 (Gomoku) — 使用 JZJSWidgetModule 新注册的 widget 类
//   JZWindow (工厂) / JZLabel / JZCanvas / JZPushButton / JZRect / JZPoint
// 运行: code_runtime --root <本目录>
//
// 注意: win / canvas / status / restart 必须处于模块顶层作用域 (ESM 模块对象会被引擎保留),
// 不能被 GC 回收, 否则会销毁底层窗口。

const GRID = 15;            // 15x15
const CELL = 40;            // 格距(逻辑像素)
const MARGIN = 20;          // 棋盘四周留白
const STATUS_H = 36;        // 顶部状态栏高度
const BTN_H = 40;           // 底部按钮高度
const BOARD = MARGIN * 2 + (GRID - 1) * CELL;
const WIN_W = BOARD;
const WIN_H = STATUS_H + BOARD + BTN_H;

const EMPTY = 0, BLACK = 1, WHITE = 2;

const board = [];
for (let i = 0; i < GRID; i++) board.push(new Array(GRID).fill(EMPTY));
let turn = BLACK;           // 黑先
let winner = EMPTY;
let over = false;

const win = new JZWindow(WIN_W, WIN_H, "五子棋");

// 顶部状态栏 (JZLabel)
const status = new JZLabel();
status.setText("轮到: 黑棋 (点击落子)");
status.setAlign(1);         // 1 = 居中
win.addWidget(status);

// 棋盘画布 (JZCanvas)
const canvas = win.createCanvas(BOARD, BOARD);

// 底部重新开局按钮 (JZPushButton)
const restart = new JZPushButton();
restart.setText("重新开始");
win.addWidget(restart);

// 用新注册的几何类做命中测试
const boardRect = new JZRect(MARGIN, MARGIN, (GRID - 1) * CELL, (GRID - 1) * CELL);

function statusText()
{
    if (over)  return winner === BLACK ? "黑棋胜利!" : (winner === WHITE ? "白棋胜利!" : "平局");
    return turn === BLACK ? "轮到: 黑棋 (点击落子)" : "轮到: 白棋 (点击落子)";
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

function reset()
{
    for (let i = 0; i < GRID; i++)
        for (let j = 0; j < GRID; j++)
            board[i][j] = EMPTY;
    turn = BLACK;
    winner = EMPTY;
    over = false;
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
    if (over || button !== 0) return;            // 仅左键
    if (!boardRect.contains(x, y)) return;       // 用 JZRect 做边界判定
    const pt = new JZPoint(x, y);                // 用 JZPoint 取坐标
    const c = Math.round((pt.x() - MARGIN) / CELL);
    const r = Math.round((pt.y() - MARGIN) / CELL);
    if (c < 0 || c >= GRID || r < 0 || r >= GRID) return;
    if (board[r][c] !== EMPTY) return;

    board[r][c] = turn;
    if (checkWin(r, c, turn)) {
        winner = turn;
        over = true;
    } else {
        // 平局判定: 棋盘满
        let full = true;
        for (let i = 0; i < GRID && full; i++)
            for (let j = 0; j < GRID; j++)
                if (board[i][j] === EMPTY) { full = false; break; }
        if (full)
            over = true;
        else
            turn = (turn === BLACK) ? WHITE : BLACK;
    }
    status.setText(statusText());
    canvas.update();
});

restart.onClicked(() => reset());

win.show();
