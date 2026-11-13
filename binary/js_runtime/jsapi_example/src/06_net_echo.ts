// 06_net_echo —— TCP 端到端: 一个回声服务 + 两个客户端(文本 / 二进制)。
//
// 编译跑起来:  <tsc> -p tsconfig.json
//              code_runtime --entry <本目录>/build/06_net_echo.js
//
// 走本机 loopback, 不依赖外网。三点值得记:
//   * net.connect / write 都是**异步**的, 结果在 'data' 事件里回来 —— 例子写在回调串里,
//     真实脚本里通常把它们包成 Promise;
//   * 打开着的套接字/服务端自己就把进程留住(对齐 Node 的 handle ref 语义), 所以等回包期间
//     不需要定时器 —— 反过来说, **忘了 close 进程就不会退出**; 不想被吊住用 socket.unref()/server.unref();
//   * 收摊很重要: 客户端 end() + 服务端 close() 都做掉, 资源才干净。
//
// 类型上的两个坑:
//   * 'data' 事件声明里给的是 Uint8Array | string(引擎按有没有 setEncoding 决定),
//     所以"没设编码就是字节"这件事要自己收窄(见下);
//   * 内置包的类型是 `declare module '<名字>'` 形式的 ambient 声明, 随工程分发的 tsgo(TS 7)
//     **不给这种导出函数的回调形参做上下文推导** —— 回调形参得自己写类型(sock: Socket),
//     类型也从同一个包名里 `import type` 取。

import net from 'net';
import type { Socket } from 'net';
import { check } from './helper/assert.js';

const PORT = 23401;

console.log('--- 06_net_echo ---');

// ---- 服务端: 来什么发回什么 ----
const server = net.createServer(function (sock: Socket) {
    sock.on('data', function (chunk: Uint8Array | string) {
        sock.write(chunk);
    });
    sock.on('error', function (e: Error) {
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
    text.on('data', function (chunk: Uint8Array | string) {
        check('文本回声一致', chunk === 'hello tcp');
        binary();   // 这一轮完了 -> 二进制那一轮
    });

    // ---- 客户端 2: 不给编码就是 Uint8Array(协议帧都是这个形态) ----
    function binary(): void {
        const bin = net.connect(PORT, '127.0.0.1', function () {
            bin.write(Uint8Array.fromHex('0102ff'));
        });
        bin.on('data', function (chunk: Uint8Array | string) {
            // 没 setEncoding => 引擎给的是字节; 收窄一下(顺带把契约钉在类型上)
            if (!(chunk instanceof Uint8Array))
                throw new Error('未设编码的 socket 应该给 Uint8Array');
            check('收到的是 Uint8Array', chunk instanceof Uint8Array);
            check('二进制回声一致', chunk.toHex() === '0102ff');

            // 收摊: 两个客户端 end、服务端 close —— 句柄都收掉, 进程才会退出(见文件头第二条)
            bin.end();
            text.end();
            server.close();
            console.log('06_net_echo done');
        });
    }
});
