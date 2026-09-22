/**
 * @jz/mqtt —— MQTT 客户端(mqtt 包风格)。
 *
 * 包装 __jz.mqtt 原子 API。默认导出 = 模块对象(MqttClient / connect)。
 */
'use strict';

import { EventEmitter } from 'events/dist';
import { toBytes } from 'internal/dist';

declare const __jz: any;

export class MqttClient extends EventEmitter {
    _h: any;
    constructor(h: any) {
        super();
        this._h = h;
        h.on('connect', () => this.emit('connect'));
        h.on('message', (topic: any, payload: any, qos: any) => {
            this.emit('message', topic, payload, { topic: topic, payload: payload, qos: qos, retain: false });
        });
        h.on('close', () => { this.emit('close'); this.emit('end'); });
    }
    publish(topic: any, message: any, a: any, b: any) {
        let qos = 0, retain = false, cb: any = null;
        if (typeof a === 'object' && a !== null) { qos = a.qos || 0; retain = !!a.retain; cb = b; }
        else if (typeof a === 'function') cb = a;
        this._h.publish(topic, toBytes(message, null), qos | 0, retain);
        if (cb) cb();
        return this;
    }
    subscribe(topic: any, a: any, b: any) {
        let qos = 0, cb: any = null;
        if (typeof a === 'object' && a !== null) { qos = a.qos || 0; cb = b; }
        else if (typeof a === 'function') cb = a;
        const list = Array.isArray(topic) ? topic : [topic];
        for (let i = 0; i < list.length; i++) this._h.subscribe(list[i], qos | 0);
        if (cb) cb(null, list.map(function (t: any) { return { topic: t, qos: qos | 0 }; }));
        return this;
    }
    unsubscribe(topic: any, cb: any) {
        const list = Array.isArray(topic) ? topic : [topic];
        for (let i = 0; i < list.length; i++) this._h.unsubscribe(list[i]);
        if (cb) cb();
        return this;
    }
    end(force: any, cb: any) {
        if (typeof force === 'function') force();
        else if (cb) cb();
        this._h.end();
        return this;
    }
    get connected() { return this._h.connected; }
}

export function connect(url: any, opts: any) {
    const o = opts || {};
    const h = __jz.mqtt.connect(String(url), {
        clientId: o.clientId || '',
        username: o.username || '',
        password: o.password || '',
        keepalive: o.keepalive || 60,
        clean: o.clean === undefined ? true : !!o.clean,
        protocolVersion: o.protocolVersion || 4,
        will: o.will ? {
            topic: o.will.topic || '',
            payload: toBytes(o.will.payload === undefined ? '' : o.will.payload, null).toBase64(),
            qos: o.will.qos || 0,
            retain: !!o.will.retain
        } : null
    });
    return new MqttClient(h);
}

const mqtt = { MqttClient, connect };
export default mqtt;
