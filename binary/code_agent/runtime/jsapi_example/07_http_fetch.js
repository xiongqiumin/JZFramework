// 07_http_fetch —— 起一个 HTTP 服务, 再用两种方式访问它: fetch 与 http.get。
//
// 跑起来:  code_runtime --entry <本文件>
//
// fetch / Headers / Response 是运行期全局(由 net 模块装载), 直接就能用;
// http 包给的是 Node 风格的 request/get + createServer。
// 同样跑本机 loopback: 用完记得 server.close()。
//
// fetch 会登记原生待办任务( await 期间宿主知道还有活), 但服务起来那一段仍然是外部事件,
// 所以这里也吊一个周期定时器保底(keepAlive), 最后一步 clear 掉 —— 理由见 06_net_echo。

import http from 'http';
import { check, keepAlive } from './helper/assert.js';

const PORT = 23402;
const base = 'http://127.0.0.1:' + PORT;
const keep = keepAlive(50);

console.log('--- 07_http_fetch ---');

// ---- 服务端 ----
const server = http.createServer(function (req, res) {
    if (req.method === 'POST') {
        res.writeHead(200, { 'Content-Type': 'text/plain' });
        res.end('echo:' + req.text);          // 服务端: 请求体已经在 req.text 里
        return;
    }
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end('{"app":"jz","ok":true}');
});

server.listen(PORT, '127.0.0.1', function () {
    console.log('http server on ' + base);

    // 用 async 把下面几步串起来写, 比回调套回调好读
    (async function main() {
        // ---- 1) fetch GET + json() ----
        const r1 = await fetch(base + '/status');
        check('GET status 200', r1.status === 200 && r1.ok);
        const j = await r1.json();
        check('GET json body', j.app === 'jz' && j.ok === true);

        // ---- 2) fetch POST(带请求体) ----
        const r2 = await fetch(base + '/report', {
            method: 'POST',
            headers: { 'Content-Type': 'text/plain' },
            body: 'pump-01:36.5',
        });
        const txt = await r2.text();
        check('POST body 回声', txt === 'echo:pump-01:36.5');

        // ---- 3) http.get(Node 形态: 事件流式读) ----
        const bodyText = await new Promise(function (resolve, reject) {
            http.get(base + '/status', function (res) {
                let acc = '';
                res.setEncoding('utf8');
                res.on('data', function (c) { acc = acc + c; });
                res.on('end', function () { resolve(acc); });
                res.on('error', reject);
            }).on('error', reject);
        });
        check('http.get 读到同样的 body', bodyText === '{"app":"jz","ok":true}');

        server.close();
        clearInterval(keep);
        console.log('07_http_fetch done');
    })();
});
