// 动态 API 示例：直接定义全局函数 onApiCall(client)，所有 /api/* 请求都会派发给它。
// 客户端应请求 http://127.0.0.1:8080/api/...
//
// client 暴露（只读）：method / path / apiPath / url / query / params / headers / body / json()
// client 暴露（可异步调用）：send / json / text / html / respond(status,ct,body) / status / set

onApiCall = function (client) {
    // 读取请求信息
    const method = client.method;
    const path = client.apiPath;   // /api 之后的部分，如 /hello
    const query = client.query;    // 对象，如 { a: '1', b: '2' }

    // 简单的路由分发（JS 自己决定怎么处理）
    if (path === '/hello') {
        client.json({ msg: 'hello from js engine', method, path });
    }
    else if (path === '/echo' && method === 'GET') {
        client.json({ query });
    }
    else if (path.startsWith('/user/')) {
        const id = path.slice('/user/'.length);
        client.json({ userId: id });
    }
    else if (path === '/time') {
        // 异步：用引擎内置 setTimeout 延迟响应
        setTimeout(() => {
            client.json({ time: Date.now() });
        }, 100);
    }
    else if (path === '/node') {
        // async/await + fetch（Node 风格）
        (async () => {
            try {
                const r = await fetch('http://127.0.0.1:8080/api/hello');
                const j = await r.json();
                client.json({ upstream: j });
            } catch (e) {
                client.respond(500, 'text/plain', 'fetch failed: ' + e);
            }
        })();
    }
    else if (path === '/ping') {
        client.set('X-Powered-By', 'JZWebView2');
        client.text('pong');
    }
    else if (path === '/echo' && method === 'POST') {
        client.json({ body: client.body, length: client.body.length });
    }
    else {
        client.respond(404, 'application/json', JSON.stringify({ error: 'not found', path }));
    }
};
