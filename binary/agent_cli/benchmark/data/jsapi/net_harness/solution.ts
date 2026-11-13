// ============================================================================
//  [你要实现的地方] 在 src/solution.ts 中实现下面的 solve（不要修改 src/main.ts）。
//
//  solve 接收已经由框架解析好的参数：
//    text : 输入文本（评测注入，_input.txt 里空格前的部分）
//    port : 监听/连接端口（_input.txt 里空格后的数字）
//
//  返回一行标记字符串（不含换行），框架会把它打印到 stdout 供评测。
//
//  注意：
//  - 运行期 cwd 是 build/；**打开着的套接字/服务端会把进程留住**（与 Node 的句柄语义一致：
//    bind/listen/connect 之后就一直算“有事要等”）—— 所以务必在返回的 Promise 里 await 完
//    所有网络往返（自测请求/响应）之后再 resolve，不要仅创建监听就立即返回。
//  - 收尾同样重要：任务做完请把用到的 socket/server **close()** 掉。不 close 进程不会退出，
//    评测会一直等到超时；确实想让它在后台活着，可以对它调 unref()。
//  - 不要自行读取 _input.txt，也不要 console.log（框架负责输出你的返回值）。
// ============================================================================
export async function solve(text: string, port: number): Promise<string>
{
    // TODO: 在这里完成网络任务并返回标记行，例如：
    //   return "HTTP=<...>";
    return "";
}
