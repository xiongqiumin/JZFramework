/// <reference path="../../types/common.d.ts" />
/// <reference path="../../types/events.d.ts" />

export type QoS = 0 | 1 | 2;

export interface IClientOptions {
    clientId?: string;
    username?: string;
    password?: string;
    /** seconds */
    keepalive?: number;
    clean?: boolean;
    protocolVersion?: 4 | 5;
    connectTimeout?: number;
    will?: {
        topic: string;
        payload: Data;
        qos?: QoS;
        retain?: boolean;
    };
}

export interface IClientPublishOptions {
    qos?: QoS;
    retain?: boolean;
}

export interface IClientSubscribeOptions {
    qos?: QoS;
}

export interface ISubscriptionGrant {
    topic: string;
    qos: QoS;
}

export interface MqttPacket {
    topic: string;
    payload: Uint8Array;
    qos: QoS;
    retain: boolean;
}

export class MqttClient extends EventEmitter {
    publish(topic: string, message: Data, cb?: (err?: Error) => void): this;
    publish(topic: string, message: Data, opts: IClientPublishOptions,
            cb?: (err?: Error) => void): this;

    subscribe(topic: string | string[],
              cb?: (err?: Error, granted?: ISubscriptionGrant[]) => void): this;
    subscribe(topic: string | string[], opts: IClientSubscribeOptions,
              cb?: (err?: Error, granted?: ISubscriptionGrant[]) => void): this;

    unsubscribe(topic: string | string[], cb?: (err?: Error) => void): this;

    end(force?: boolean, cb?: () => void): this;

    readonly connected: boolean;

    on(event: 'connect', listener: () => void): this;
    on(event: 'message',
       listener: (topic: string, payload: Uint8Array, packet: MqttPacket) => void): this;
    on(event: 'close' | 'end', listener: () => void): this;
    on(event: 'error', listener: (err: Error) => void): this;
    on(event: string, listener: (...args: any[]) => void): this;
}

/** url: mqtt://host:port or mqtts://host:port */
export function connect(url: string, options?: IClientOptions): MqttClient;

/** default export = the module object (same usage as Node: import x from 'mqtt') */
declare const _default: { MqttClient: typeof MqttClient, connect: typeof connect };
export default _default;
