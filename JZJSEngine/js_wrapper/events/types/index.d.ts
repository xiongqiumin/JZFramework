/// <reference path="../../types/common.d.ts" />

/**
 * events —— Node 的 `events` 模块(EventEmitter 类)。
 *
 * 默认导出与具名导出都是这个类:
 *   import EventEmitter from 'events';
 *   import { EventEmitter } from 'events';
 *
 * ⚠ 运行期它同时是**全局 EventEmitter**(见共享声明 ../../types/events.d.ts) ——
 *   本文件是模块形式的那份引用(供裸名 'events' 用), 两边成员要一起改。
 *
 * 与 Node 一致: 没有 'error' 监听器时 emit('error') 会抛异常(Unhandled 'error' event)。
 */
export class EventEmitter {
    constructor();

    on(event: string, listener: (...args: any[]) => void): this;
    once(event: string, listener: (...args: any[]) => void): this;
    off(event: string, listener?: (...args: any[]) => void): this;
    /** off 的别名(Node 兼容) */
    removeListener(event: string, listener?: (...args: any[]) => void): this;
    removeAllListeners(event?: string): this;
    emit(event: string, ...args: any[]): boolean;
    listenerCount(event: string): number;
    /** 返回副本(Node 同名语义) */
    listeners(event: string): Function[];
    eventNames(): string[];
}

export default EventEmitter;
