/// <reference path="common.d.ts" />
/// <reference path="events.d.ts" />
/// <reference path="globals.d.ts" />
/// <reference path="console.d.ts" />
/// <reference path="fetch.d.ts" />
/// <reference path="timers.d.ts" />

/**
 * JZ JS 引擎的类型声明入口(只有"跨模块共享的真全局"部分)。
 *
 * 本目录(JZJSEngine/js_wrapper/types/)只放不属于任何单个模块的全局声明:
 *   - console / fetch / Headers / Response / RequestInit (引擎注册到 globalThis 的全局)
 *   - sleep / setInterval / setTimeout / clearInterval / clearTimeout / engine (同上是引擎装的全局)
 *   - URL / URLSearchParams / WebSocket / EventEmitter (运行期就是全局对象/全局类)
 *   - common.d.ts: 公共类型别名(Data / Chunk / DataEncoding) + Uint8Array 扩展方法
 *
 * ⭐ 布局(2026-09-22 起): **一个名字 = 一个包目录, 目录名就是 import 的名字**
 *   js_wrapper/<名字>/index.ts            运行期实现(tsc -> <名字>/dist/index.js)
 *   js_wrapper/<名字>/types/index.d.ts    模块类型(单一来源, 与实现同目录)
 *   js_wrapper/<名字>/types/ambient.d.ts  只声明裸名并转发(declare module '<名字>')
 *   js_wrapper/types/                     不属于任何名字的共享声明(本目录: 全局/console/fetch/...)
 *   js_wrapper/internal/                  **不是**内置名的内部工具(各包实现互相 import 用)
 * 于是"名字 → 类型文件 → 运行期入口"三者同名: 想查 http 有哪些成员, 就看
 * jz_types/http/types/index.d.ts(目录名就是 import 的名字, 不用再记"http 藏在 net 包里")。
 *
 * 16 个内置名(与 JZJSEngine.cpp 的 builtinModules 一一对应):
 *   net / dgram / http / mqtt / ws / url / events / fs / path / process / crypto / bytes /
 *   os / serialport / modbus / sqlite(控件族另有 widget; 这些名字都是"包目录名")
 * 每个名字都是"**默认导出 = 模块对象** + **具名导出 = 各成员**", 两种写法都成立:
 *   import http from 'http';                 import { createServer } from 'http';
 *   import { DatabaseSync } from 'sqlite';   import fs from 'node:fs';
 * (ambient `declare module '<裸名>'` 只能写在全局脚本文件里, 所以每个包多一个 ambient.d.ts;
 *  模块形式的 <module>.d.ts 若混写 declare module 会被当成"模块增强"而找不到目标。)
 *
 * Types are imported as type-only, so they are erased at runtime and never load a module:
 *
 *   import type { Socket } from 'net';
 *   import type { Stats } from 'fs';
 *
 * 运行期用包 dist(仍然是 ES module), 只支持 ESM 写法:
 *
 *   (1) Node 风格裸名(推荐, 引擎内置名字表, 同 Node 的内置模块; 'node:' 前缀也认)
 *       import http from 'http';
 *       import fs from 'node:fs';
 *
 *   (2) 相对路径(jz_types / jz_js 由 initProject 一起拷进用户工程, 按工程内相对路径 import)
 *       import http from '../jz_js/http/dist/index.js';
 *       import fs from '../jz_js/fs/dist/index.js';
 *       import { SerialPort } from '../jz_js/serialport/dist/index.js';
 *
 * ⭐ 名字 = 契约: 每个名字都必须在**该包的** types/ambient.d.ts 里有 `declare module '<名字>'`,
 *   类型与运行按同一个名字对齐(与 @types/node 按同名给类型是一回事); 缺 ambient 的症状是
 *   TS2307 —— 编译不过、运行期却是好的, 模型会被白绊一次。
 *
 * ⚠ 不支持 CommonJS: 没有 require, 也不认 module.exports / exports。
 *
 * 部署产物布局(package.py 打包; 与源码 js_wrapper/ 同构 => 相对引用原样可解析):
 *   <exe>/jz_engine/jz_types/types/**         <- js_wrapper/types(本目录, 只有共享/全局声明)
 *   <exe>/jz_engine/jz_types/<名字>/types/**  <- js_wrapper/<名字>/types(模块类型 + ambient.d.ts)
 *   <exe>/jz_engine/jz_js/<名字>/dist/**      <- js_wrapper/<名字>/dist
 *
 * Usage in a TS project:
 *   1. copy the deployed jz_types/ folder to <project>/jz_types/
 *   2. include it in tsconfig.json:
 *        "include": ["src/**\/*.ts", "jz_types/**\*.d.ts"]
 *   3. set "lib": ["ES2022"] (without DOM), otherwise the global WebSocket,
 *      fetch, Response, Headers, URL and URLSearchParams declared here collide
 *      with lib.dom.d.ts and TS reports duplicate identifier errors.
 */
