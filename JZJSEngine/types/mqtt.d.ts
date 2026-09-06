/// <reference path="common.d.ts" />
/// <reference path="events.d.ts" />

/**
 * mqtt - aligned with MQTT.js (npm "mqtt"), the de facto standard client.
 * Node has no built-in MQTT module.
 *
 * Differences:
 * - no auto reconnect / offline queue, so 'reconnect'/'offline' are absent.
 * - no 'packetsend'/'packetreceive' events.
 */
declare module 'mqtt' {
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
}

/** the engine injects the same object into the global scope */
declare const mqtt: typeof import('mqtt');
