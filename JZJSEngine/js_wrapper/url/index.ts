/**
 * @jz/url —— Node `url` 模块(legacy parse) + Web 平台全局 URL / URLSearchParams。
 *
 * URL / URLSearchParams 是 **Web 标准全局**(对齐 Node: 启动即有的全局, 无需 import),
 * 本模块装载时把它们挂到 globalThis; 同时也作为具名导出暴露
 * (⇒ `import { URL } from 'url'` 也能用, Node 里 url.URL 是存在的)。
 * 默认导出 = 模块对象(URL / URLSearchParams / parse)。
 */
'use strict';

declare const __jz: any;

export class URLSearchParams {
    _m: any;
    constructor(init: any) {
        this._m = Object.create(null);
        if (!init) return;
        if (typeof init === 'string') {
            const s = init.charAt(0) === '?' ? init.slice(1) : init;
            if (!s) return;
            const parts = s.split('&');
            for (let i = 0; i < parts.length; i++) {
                if (!parts[i]) continue;
                const p = parts[i].indexOf('=');
                if (p < 0) this._m[decodeURIComponent(parts[i])] = '';
                else this._m[decodeURIComponent(parts[i].slice(0, p))] = decodeURIComponent(parts[i].slice(p + 1));
            }
        } else {
            for (const k in init) this._m[k] = String(init[k]);
        }
    }
    get(n: any) { const v = this._m[n]; return v === undefined ? null : v; }
    has(n: any) { return this._m[n] !== undefined; }
    set(n: any, v: any) { this._m[n] = String(v); }
    append(n: any, v: any) {
        this._m[n] = (this._m[n] === undefined) ? String(v) : this._m[n] + ',' + String(v);
    }
    delete(n: any) { delete this._m[n]; }
    keys() { return Object.keys(this._m); }
    values() { const r: any[] = []; for (const k in this._m) r.push(this._m[k]); return r; }
    entries() { const r: any[] = []; for (const k in this._m) r.push([k, this._m[k]]); return r; }
    forEach(cb: any) { for (const k in this._m) cb(this._m[k], k, this); }
    toString() {
        const r: any[] = [];
        for (const k in this._m) r.push(encodeURIComponent(k) + '=' + encodeURIComponent(this._m[k]));
        return r.join('&');
    }
}
(globalThis as any).URLSearchParams = URLSearchParams;

export class URL {
    _p: any;
    href: any;
    searchParams: any;
    constructor(u: any) {
        const p = __jz.url.parse(String(u));
        this._p = p;
        this.href = p.href;
        this.searchParams = new URLSearchParams(p.search);
    }
    get protocol() { return this._p.protocol; }
    get hostname() { return this._p.hostname; }
    get host() { return this._p.host; }
    get port() { return this._p.port; }
    get pathname() { return this._p.pathname; }
    get search() { return this._p.search; }
    get path() { return this._p.path; }
    toString() { return this.href; }
}
(globalThis as any).URL = URL;

/** legacy Node `url.parse()`(只是把 __jz.url.parse 的结果原样给出, 不构造 URL 对象) */
export function parse(u: any) { return __jz.url.parse(String(u)); }

const url = { URL, URLSearchParams, parse };
export default url;
