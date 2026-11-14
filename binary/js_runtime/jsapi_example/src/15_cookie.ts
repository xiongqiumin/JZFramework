// 15_cookie —— HTTP 服务端如何收发 Cookie(全部按 Node 原生形态, 没有框架特有 API)。
//
// 编译跑起来:  <tsc> -p tsconfig.json
//              code_runtime --entry <本目录>/build/15_cookie.js
//
// Node 原生只有两件事:
//   发: res.setHeader('Set-Cookie', 'k=v; Path=/') 或数组(多条)
//   收: 自己拆 req.headers.cookie(Node 里没有 req.cookies, 那是 Express)
//
// 客户端侧不用管: 运行时自带 cookie jar —— 收到 Set-Cookie 会自动存, 后续同域请求自动带上。

import http from 'http';
import type { ServerRequest, ServerResponse } from 'http';
import { check, eq } from './helper/assert.js';

const PORT = 23415;
const base = 'http://127.0.0.1:' + PORT;

console.log('--- 15_cookie ---');

// Node 里取 cookie 的标准写法: 拆 "a=1; b=2"
function parseCookies(header: string): Record<string, string> {
    const out: Record<string, string> = {};
    const text = header || '';
    for (const part of text.split(';')) {
        const i = part.indexOf('=');
        if (i < 0) continue;
        const k = part.slice(0, i).trim();
        let v = part.slice(i + 1).trim();
        if (v.length >= 2 && v.charAt(0) === '"' && v.charAt(v.length - 1) === '"')
            v = v.slice(1, v.length - 1);
        if (k) out[k] = v;
    }
    return out;
}

const server = http.createServer(function (req: ServerRequest, res: ServerResponse) {
    if (req.url === '/login') {
        // 多条 Set-Cookie: 数组(与 Node 一致)。会话给 HttpOnly/SameSite, 偏好不限。
        res.setHeader('Set-Cookie', [
            'sid=user-42; Path=/; Max-Age=3600; HttpOnly; SameSite=Lax',
            'theme=dark; Path=/',
        ]);
        res.writeHead(200, { 'Content-Type': 'text/plain' });
        res.end('login ok');
        return;
    }

    if (req.url === '/logout') {
        // Max-Age=0 -> 客户端删掉这个 cookie
        res.setHeader('Set-Cookie', 'sid=; Path=/; Max-Age=0');
        res.writeHead(200, { 'Content-Type': 'text/plain' });
        res.end('bye');
        return;
    }

    // 有 sid 就当已登录, 否则匿名 —— "按 cookie 区分用户"的最小形态
    const cookies = parseCookies(req.headers.cookie || '');
    const sid = cookies['sid'] || '';
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.end(sid ? ('user:' + sid) : 'anonymous');
});

server.listen(PORT, '127.0.0.1', function () {
    (async function main(): Promise<void> {
        // 1) 没带 cookie -> 匿名
        const r1 = await fetch(base + '/whoami');
        eq('无 cookie 时的身份', await r1.text(), 'anonymous');

        // 2) 登录: 服务端下发两条 Set-Cookie
        //    ⚠ 客户端 Headers 只保留最后一条同名头, 这里只断言"确实下发了"
        const r2 = await fetch(base + '/login');
        const setCookie = r2.headers.get('set-cookie') || '';
        check('Set-Cookie 已下发', setCookie.length > 0);

        // 3) 第二次请求不用自己带 Cookie: 运行时的 cookie jar 会自动回送
        const r3 = await fetch(base + '/whoami');
        eq('自动带 cookie 时的身份', await r3.text(), 'user:user-42');

        // 4) 显式设置的 Cookie 头优先于 jar(便于模拟别的会话)
        const r4 = await fetch(base + '/whoami', { headers: { Cookie: 'sid=other' } });
        eq('显式 Cookie 头优先', await r4.text(), 'user:other');

        // 5) 登出: 服务端下发 Max-Age=0, 客户端应把 sid 删掉
        await fetch(base + '/logout');
        const r5 = await fetch(base + '/whoami');
        eq('登出后的身份', await r5.text(), 'anonymous');

        server.close();
        console.log('15_cookie done');
    })();
});
