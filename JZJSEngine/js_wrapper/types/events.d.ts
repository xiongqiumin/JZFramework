/// <reference path="common.d.ts" />

/**
 * 事件发射器，语义对齐 Node 的 events.EventEmitter。
 * net.Socket / net.Server / dgram.Socket / http.ClientRequest /
 * http.IncomingMessage / WebSocket / ws.WebSocketServer / mqtt.MqttClient 均继承它。
 */
/** @final */
declare class EventEmitter {
    on(event: string, listener: (...args: any[]) => void): this;
    once(event: string, listener: (...args: any[]) => void): this;
    off(event: string, listener?: (...args: any[]) => void): this;
    /** off 的别名(Node 兼容) */
    removeListener(event: string, listener?: (...args: any[]) => void): this;
    removeAllListeners(event?: string): this;
    emit(event: string, ...args: any[]): boolean;
    listenerCount(event: string): number;
    eventNames(): string[];
}

/**
 * 与 Node 的差异：没有 'error' 监听器时，Node 会 throw；
 * 本实现改为输出到 console.error 并上报引擎错误链，不会抛出未捕获异常。
 */
