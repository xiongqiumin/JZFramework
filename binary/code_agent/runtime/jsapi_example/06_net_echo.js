// 06_net_echo —— TCP 端到端: 一个回声服务 + 两个客户端(文本 / 二进制)。
//
// 跑起来:  code_runtime --entry <本文件>
//
// 走本机 loopback, 不依赖外网。三点值得记:
//   * net.connect / write 都是**异步**的, 结果在 'data' 事件里回来 —— 例子写在回调串里,
//     真实脚本里通常把它们包成 Promise;
//   * 等 TCP 回包期间引擎"没有待办任务", 所以要吊一个周期定时器把进程留住(keepAlive),
//     最后一步 clear 掉 —— 忘了吊, 宿主会在回包之前就退出;
//   * 收摊很重要: 客户端 end() + 服务端 close() 都做掉, 资源才干净。

import net from 'net';
import { check, keepAlive } from './helper/assert.js';

const PORT = 23401;
const keep = keepAlive(50);     // ⚠ 见文件头: 本例全程在等外部回调

console.log('--- 06_net_echo ---');

// ---- 服务端: 来什么发回什么 ----
const server = net.createServer(function (sock) {
    sock.on('data', function (chunk) {
        sock.write(chunk);
    });
    sock.on('error', function (e) {
        console.log('[server socket error] ' + e.message);
    });
});

server.listen(PORT, '127.0.0.1', function () {
    console.log('listening on 127.0.0.1:' + PORT);

    // ---- 客户端 1: setEncoding('utf8') 之后 'data' 给的是字符串 ----
    const text = net.connect(PORT, '127.0.0.1', function () {
        text.write('hello tcp');
    });
    text.setEncoding('utf8');
    text.on('data', function (s) {
        check('文本回声一致', s === 'hello tcp');
        binary();   // 这一轮完了 -> 二进制那一轮
    });

    // ---- 客户端 2: 不给编码就是 Uint8Array(协议帧都是这个形态) ----
    function binary() {
        const bin = net.connect(PORT, '127.0.0.1', function () {
            bin.write(Uint8Array.fromHex('0102ff'));
        });
        bin.on('data', function (u8) {
            check('收到的是 Uint8Array', u8 instanceof Uint8Array);
            check('二进制回声一致', u8.toHex() === '0102ff');

            bin.end();
            text.end();
            server.close();
            clearInterval(keep);        // ⚠ 必须收掉, 否则进程不退出
            console.log('06_net_echo done');
        });
    }
});
