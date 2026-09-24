/**
 * five_chess_server —— benchmark 用例 `five_chess_server` 的参考实现（随用例放在数据目录里）。
 *
 * ⚠ 本文件不是工程文件：**用例本身仍要求模型自己产出**（initProject 的临时工程 + 模型写代码）。
 *   这份只作参考答案 / 回归自测用 —— 用时拷成工程根的 `src/main.ts` 再编译运行。
 *
 * 15x15 五子棋对战服务器（单进程、内存态、多房间隔离），监听 3900，HTTP JSON 接口：
 *   POST /create_room {"name":"x"}                      -> {room_id, token}   创建者执黑
 *   GET  /list_rooms                                    -> {rooms:[{room_id,name,has_white}]}
 *   POST /join_room   {"room_id":<int>}                 -> {token}            加入者执白
 *   GET  /get_match?room_id=<int>&token=<str>           -> {board,turn,last_move,result,move_count}
 *   POST /play        {"room_id","token","x","y"}       -> {ok,error,result}
 * 规则：黑先手；任意方向连成 5 子者胜；棋盘下满无胜者为和；非法落子 ok=false 且不动棋盘。
 *
 * 运行期依赖：引擎的模块机制是 ESM（**没有 require**，写了就是 ReferenceError）——
 *   内置模块写裸名即可：import http from 'http'（'node:http' 也认）；
 *   一个名字 = 一个包目录，**默认导入与具名导入都成立**：
 *     import http from 'http' 与 import { createServer } from 'http' 都可以。
 *   也可以按工程内相对路径 import（jz_js/<名字>/dist/index.js，默认导出 = 模块对象）。
 *   工程根下的 jz_js/ 由 initProject 从 <exe>/jz_engine/jz_js 拷入（打包/发布产物里同目录）。
 *   另外：退出/保活是 Node 的 handle ref 语义（2026-09-24 对齐）——**监听中的 server 自己就把
 *   进程留住**，所以这个常驻服务不需要任何心跳定时器；反过来，想让某个句柄不吊住进程就 unref()。
 *
 * 自测（工程根执行；jz_types、jz_js 需先按 initProject 的布局就位）：
 *   copy <本文件> <工程根>/src/main.ts
 *   <exe>/ts_compiler/tsc/tsc.exe -p tsconfig.json              # 产物 -> build/main.js
 *   python <data>/five_chess_server/five_chess_server_verify.py \
 *          --runtime <exe>/code_runtime.exe --build <工程根>/build --port 3900
 *   （工程骨架 = initProject 生成的标准 tsconfig：target ES2022 / module esnext /
 *     moduleResolution bundler / lib ES2025 / strict / sourceMap / outDir build / rootDir src）
 */

import http from 'http';

// ============================================================================
// 常量
// ============================================================================
const SIZE = 15;

const EMPTY = 0;
const BLACK = 1;
const WHITE = 2;

const PLAYING = 0;
const BLACK_WIN = 1;
const WHITE_WIN = 2;
const DRAW = 3;

const PORT = 3900;

// ============================================================================
// 引擎 http 适配层的入参/出参（运行期包声明为 any，这里只声明用到的字段，
// 拿到 tsconfig 的 strict 检查；类型来自 jz_types：import type { ... } from 'http'）
// ============================================================================
interface HttpReq {
    method?: string;
    url?: string;
    headers?: any;
    body?: Uint8Array;
    text?: string;
}

interface HttpRes {
    writeHead(status: number, headers?: Record<string, string>): void;
    end(body?: string): void;
}

// ============================================================================
// 房间
// ============================================================================
class Room {
    id: number;
    name: string;
    blackToken: string;
    whiteToken: string | null;

    /** board[y][x]：y = 行(0~14)，x = 列(0~14)，0 空 / 1 黑 / 2 白 */
    board: number[][];

    /** 轮到谁：0 = 黑，1 = 白 */
    turn: number;
    result: number;
    moveCount: number;

    lastX: number | null;
    lastY: number | null;
    lastColor: number;

    constructor(id: number, name: string, blackToken: string) {
        this.id = id;
        this.name = name;
        this.blackToken = blackToken;
        this.whiteToken = null;

        this.board = [];
        for (let y = 0; y < SIZE; y++) {
            const row: number[] = [];
            for (let x = 0; x < SIZE; x++)
                row.push(EMPTY);
            this.board.push(row);
        }

        this.turn = 0;          // 黑先手
        this.result = PLAYING;
        this.moveCount = 0;
        this.lastX = null;
        this.lastY = null;
        this.lastColor = EMPTY;
    }

    /** token 对应的棋子颜色；不属于本房间返回 EMPTY */
    seatOf(token: string): number {
        if (token.length > 0 && token === this.blackToken)
            return BLACK;
        if (this.whiteToken !== null && token === this.whiteToken)
            return WHITE;
        return EMPTY;
    }

    /** 该颜色是否轮到自己 */
    isTurnOf(color: number): boolean {
        return this.turn === (color === BLACK ? 0 : 1);
    }
}

const rooms = new Map<number, Room>();
let nextRoomId = 1;
let tokenSeq = 0;

function newToken(): string {
    tokenSeq++;
    const rnd = Math.floor(Math.random() * 0x7fffffff).toString(36);
    return 't' + tokenSeq.toString(36) + rnd + Date.now().toString(36);
}

// ============================================================================
// HTTP 小工具
// ============================================================================
function sendJson(res: HttpRes, body: any): void {
    res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
    res.end(JSON.stringify(body));
}

function pathOf(url: string): string {
    const q = url.indexOf('?');
    return q < 0 ? url : url.substring(0, q);
}

function queryOf(url: string): URLSearchParams {
    const q = url.indexOf('?');
    return new URLSearchParams(q < 0 ? '' : url.substring(q + 1));
}

/** 读请求体（引擎侧已整包收好，req.text 是解码后的字符串；无体/非 JSON 一律当空对象） */
function bodyOf(req: HttpReq): any {
    let text = '';
    try {
        text = (typeof req.text === 'string') ? req.text : '';
    } catch (err) {
        text = '';
    }
    if (text.length === 0)
        return {};
    try {
        const v: any = JSON.parse(text);
        return (v && typeof v === 'object') ? v : {};
    } catch (err) {
        return {};
    }
}

function toInt(v: any, fallback: number): number {
    if (typeof v === 'number' && isFinite(v))
        return Math.trunc(v);
    if (typeof v === 'string' && v.length > 0) {
        const n = parseInt(v, 10);
        if (!isNaN(n))
            return n;
    }
    return fallback;
}

function toStr(v: any): string {
    return (typeof v === 'string') ? v : (v === null || v === undefined ? '' : String(v));
}

/** 以 (x,y) 为中心，横/竖/斜四方向是否连成 5 子 */
function winsAt(board: number[][], x: number, y: number, color: number): boolean {
    const dirs = [[1, 0], [0, 1], [1, 1], [1, -1]];
    for (const d of dirs) {
        const dx = d[0], dy = d[1];
        let count = 1;
        for (const sign of [1, -1]) {
            for (let step = 1; step < 5; step++) {
                const nx = x + dx * step * sign;
                const ny = y + dy * step * sign;
                if (nx < 0 || nx >= SIZE || ny < 0 || ny >= SIZE)
                    break;
                if (board[ny][nx] !== color)
                    break;
                count++;
            }
        }
        if (count >= 5)
            return true;
    }
    return false;
}

// ============================================================================
// 接口实现
// ============================================================================

// POST /create_room {"name":"x"} -> {room_id, token} —— 创建者执黑
function handleCreateRoom(req: HttpReq, res: HttpRes): void {
    const body = bodyOf(req);
    const name = toStr(body.name);
    const id = nextRoomId++;
    const room = new Room(id, name.length > 0 ? name : ('room ' + id), newToken());
    rooms.set(id, room);
    console.log('[create_room] id=' + id + ' name=' + room.name);
    sendJson(res, { ok: true, room_id: id, token: room.blackToken, name: room.name });
}

// GET /list_rooms -> {rooms:[{room_id, name, has_white}]}
function handleListRooms(res: HttpRes): void {
    const list: any[] = [];
    rooms.forEach((room) => {
        list.push({ room_id: room.id, name: room.name, has_white: room.whiteToken !== null });
    });
    sendJson(res, { ok: true, rooms: list });
}

// POST /join_room {"room_id":<int>} -> {token} —— 加入者执白
function handleJoinRoom(req: HttpReq, res: HttpRes): void {
    const body = bodyOf(req);
    const id = toInt(body.room_id, -1);
    const room = rooms.get(id);
    if (!room) {
        sendJson(res, { ok: false, error: 'room not found: ' + id });
        return;
    }
    if (room.whiteToken !== null) {
        sendJson(res, { ok: false, error: 'room already has a white player' });
        return;
    }
    room.whiteToken = newToken();
    console.log('[join_room] id=' + id);
    sendJson(res, { ok: true, room_id: id, token: room.whiteToken });
}

// GET /get_match?room_id=<int>&token=<str>
function handleGetMatch(req: HttpReq, res: HttpRes): void {
    const q = queryOf(req.url || '');
    const id = toInt(q.get('room_id'), -1);
    const room = rooms.get(id);
    if (!room) {
        sendJson(res, { ok: false, error: 'room not found: ' + id });
        return;
    }
    if (room.seatOf(toStr(q.get('token'))) === EMPTY) {
        sendJson(res, { ok: false, error: 'invalid token' });
        return;
    }
    sendJson(res, {
        ok: true,
        room_id: room.id,
        name: room.name,
        board: room.board,               // 15x15：board[y][x]，0 空 / 1 黑 / 2 白
        turn: room.turn,                 // 0 黑 / 1 白
        last_move: { x: room.lastX, y: room.lastY, color: room.lastColor },
        result: room.result,             // 0 进行中 / 1 黑胜 / 2 白胜 / 3 和
        move_count: room.moveCount,
        has_white: room.whiteToken !== null
    });
}

// POST /play {"room_id","token","x","y"} -> {ok, error, result}
function handlePlay(req: HttpReq, res: HttpRes): void {
    const body = bodyOf(req);
    const id = toInt(body.room_id, -1);
    const room = rooms.get(id);
    if (!room) {
        sendJson(res, { ok: false, error: 'room not found: ' + id, result: PLAYING });
        return;
    }

    const color = room.seatOf(toStr(body.token));
    if (color === EMPTY) {
        sendJson(res, { ok: false, error: 'invalid token', result: room.result });
        return;
    }
    if (room.result !== PLAYING) {
        sendJson(res, { ok: false, error: 'game is over', result: room.result });
        return;
    }
    if (!room.isTurnOf(color)) {
        sendJson(res, { ok: false, error: 'not your turn', result: room.result });
        return;
    }

    const x = toInt(body.x, -1);
    const y = toInt(body.y, -1);
    if (x < 0 || x >= SIZE || y < 0 || y >= SIZE) {
        sendJson(res, { ok: false, error: 'out of range: x=' + x + ' y=' + y, result: room.result });
        return;
    }
    if (room.board[y][x] !== EMPTY) {
        sendJson(res, { ok: false, error: 'cell occupied: x=' + x + ' y=' + y, result: room.result });
        return;
    }

    // ---- 落子（走到这里才改棋盘：上面所有拒绝分支都不动状态）----
    room.board[y][x] = color;
    room.moveCount++;
    room.lastX = x;
    room.lastY = y;
    room.lastColor = color;

    if (winsAt(room.board, x, y, color)) {
        room.result = (color === BLACK) ? BLACK_WIN : WHITE_WIN;
        console.log('[play] room=' + id + ' color=' + color + ' WIN at ' + x + ',' + y);
    } else if (room.moveCount >= SIZE * SIZE) {
        room.result = DRAW;          // 棋盘下满无胜者
    } else {
        room.turn = (color === BLACK) ? 1 : 0;   // 换手
    }

    sendJson(res, {
        ok: true,
        room_id: id,
        result: room.result,
        move_count: room.moveCount,
        turn: room.turn,
        last_move: { x: room.lastX, y: room.lastY, color: room.lastColor }
    });
}

// ============================================================================
// 服务器
// ============================================================================
const server = http.createServer((req: HttpReq, res: HttpRes) => {
    const url = (typeof req.url === 'string' && req.url.length > 0) ? req.url : '/';
    const path = pathOf(url);
    const method = (req.method || 'GET').toUpperCase();

    try {
        if (path === '/create_room' && method === 'POST') { handleCreateRoom(req, res); return; }
        if (path === '/list_rooms' && method === 'GET') { handleListRooms(res); return; }
        if (path === '/join_room' && method === 'POST') { handleJoinRoom(req, res); return; }
        if (path === '/get_match' && method === 'GET') { handleGetMatch(req, res); return; }
        if (path === '/play' && method === 'POST') { handlePlay(req, res); return; }

        // 未知路径/方法：仍以 200 + JSON 回，调用方按 ok=false 处理
        sendJson(res, { ok: false, error: 'not found: ' + method + ' ' + path });
    } catch (err) {
        console.log('[error] ' + String(err));
        sendJson(res, { ok: false, error: 'internal error: ' + String(err) });
    }
});

server.listen(PORT, () => {
    console.log('[five_chess_server] listening on http://127.0.0.1:' + PORT);
});

// 不需要心跳保活：监听中的 server 是 Node 语义下的"ref 着的句柄"，自己就会把进程留住
// （顶层跑完也不会退；进程要退得靠 close() 或外部 kill）。要让它不吊住进程才用 server.unref()。
