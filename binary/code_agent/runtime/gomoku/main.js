// 五子棋 (Gomoku) — 纯 JS + Canvas demo，由 code_runtime 运行。
// 用法: code_runtime --root <本目录>
//
// 依赖运行期原生桥 (JZJSWidgetModule) 提供的全局:
//   JZWindow(w, h, title) -> 窗口句柄
//   win.createCanvas(w, h) -> 画布句柄 (已加入窗口布局)
//   canvas.setOnPaint((ctx) => ...)    ctx 方法映射到 JZPainter
//   canvas.setOnMouse((x, y, button) => ...)
//   canvas.update()                    触发重绘
//   win.show()
//
// 注意: win / canvas 必须处于模块顶层作用域 (ESM 模块对象会被引擎保留),
// 不能被 GC 回收, 否则会销毁底层窗口。

const GRID = 15;            // 15x15
const CELL = 40;            // 格距(逻辑像素)
const MARGIN_X = 20;        // 左右留白
const MARGIN_Y = 50;        // 顶部状态行高度
const STONE_R = 17;         // 棋子半径

const W = MARGIN_X * 2 + (GRID - 1) * CELL;
const H = MARGIN_Y + (GRID - 1) * CELL + MARGIN_X;

const EMPTY = 0, BLACK = 1, WHITE = 2;

const board = [];
for (let i = 0; i < GRID; i++) board.push(new Array(GRID).fill(EMPTY));
let turn = BLACK;           // 黑先
let winner = EMPTY;
let over = false;

const win = JZWindow(W, H, "五子棋");
const canvas = win.createCanvas(W, H);

function status()
{
    if (over)  return winner === BLACK ? "黑棋胜利!" : (winner === WHITE ? "白棋胜利!" : "平局");
    return turn === BLACK ? "轮到: 黑棋 (点击落子)" : "轮到: 白棋 (点击落子)";
}

function draw(ctx)
{
    // 背景
    ctx.fillRect(0, 0, W, H, "#e3b572");

    // 状态行
    ctx.setPen("#1d1207", 1);
    ctx.setFont("Microsoft YaHei", 18);
    ctx.drawText(MARGIN_X, 30, status());

    const x0 = MARGIN_X, y0 = MARGIN_Y;

    // 棋盘网格
    ctx.setPen("#3a2a12", 1);
    for (let i = 0; i < GRID; i++)
    {
        const gx = x0 + i * CELL, gy = y0 + i * CELL;
        ctx.drawLine(x0, gy, x0 + (GRID - 1) * CELL, gy);
        ctx.drawLine(gx, y0, gx, y0 + (GRID - 1) * CELL);
    }

    // 星位
    const stars = [[3,3],[3,7],[3,11],[7,3],[7,7],[7,11],[11,3],[11,7],[11,11]];
    for (const [sr, sc] of stars)
        ctx.fillEllipse(x0 + sc * CELL - 3, y0 + sr * CELL - 3, 6, 6, "#3a2a12");

    // 棋子
    for (let r = 0; r < GRID; r++)
    {
        for (let c = 0; c < GRID; c++)
        {
            const v = board[r][c];
            if (v === EMPTY) continue;
            const cx = x0 + c * CELL, cy = y0 + r * CELL;
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

canvas.setOnPaint((ctx) => draw(ctx));

canvas.setOnMouse((x, y, button) => {
    if (over) return;
    if (button !== 0) return;   // 仅左键
    const c = Math.round((x - MARGIN_X) / CELL);
    const r = Math.round((y - MARGIN_Y) / CELL);
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
    canvas.update();
});

win.show();
