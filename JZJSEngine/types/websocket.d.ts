// 全局 WebSocket 类见 globals.d.ts; 这里只转发 'ws' 模块(WebSocketServer)。
declare module 'ws' {
    export * from '../js_wrapper/net/types/websocket';
}
