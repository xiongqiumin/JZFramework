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
 * 各 Node 风格模块的**裸名 ambient 声明**(declare module 'fs' | 'net' | ...)与模块类型
 * 本身都不放在这里, 而是就近放在各自包里(单一来源, 不与实现脱节):
 *   - core/types/ambient.d.ts        -> fs / path / process / crypto / bytes   (模块类型见同目录 <module>.d.ts)
 *   - net/types/ambient.d.ts         -> net / dgram / http / mqtt / url / os / ws / events
 *   - serialport/types/ambient.d.ts  -> serialport (+ 运行期全局 serialport)
 *   - sqlite/types/ambient.d.ts      -> sqlite
 *   - modbus/types/ambient.d.ts      -> modbus
 * 控件侧(全局类, 无模块)见 widget/types/(json 见 widget 包)。
 * (ambient `declare module '<裸名>'` 只能写在全局脚本文件里, 所以每包多一个 ambient.d.ts;
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
 *       import { http } from '../jz_js/net/dist/index.js';
 *       import { fs, path } from '../jz_js/core/dist/index.js';
 *       import { SerialPort } from '../jz_js/serialport/dist/index.js';
 *
 * 内置的裸名有: net / dgram / http / mqtt / ws / url / events / fs / path / process /
 * crypto / bytes / os / serialport / modbus / sqlite(详见 JZJSEngine.cpp 的 builtinModules)。
 * ⭐ 名字 = 契约: 每个名字都必须在**该包** types/ambient.d.ts 里有 `declare module '<名字>'`,
 *   类型与运行按同一个名字对齐(与 @types/node 按同名给类型是一回事); 缺 ambient 的症状是
 *   TS2307 —— 编译不过、运行期却是好的, 模型会被白绊一次。
 * 注意区别: 裸名导入拿到的是**那个模块自己**(如 http 对象, http.createServer);
 * 相对路径导入必须再解一层具名导出({ http }), 因为它导入的是整个包出口。
 *
 * ⚠ 不支持 CommonJS: 没有 require, 也不认 module.exports / exports。
 *   导入形态按名字分两类:
 *     - 包级入口(有默认导出): import http from 'http'; import fs from 'node:fs';
 *     - namespace 形态(**无 default**, 必须具名导入): import { DatabaseSync } from 'sqlite';
 *       import { ByteReader, ByteWriter, hexOf } from 'bytes';
 *   ⚠ 目前包级入口只转发"包出口的具名导出", 并不是 Node 意义上的子模块成员:
 *     `import { createServer } from 'http'` 编译能过、运行报 "Could not find export 'createServer'"。
 *     想拿子模块成员, 现阶段的正确写法是默认导入再用成员(http.createServer)。
 *
 * 部署产物布局(package.py 打包; 与源码 js_wrapper/ 同构 => 相对引用原样可解析):
 *   <exe>/jz_engine/jz_types/types/**        <- js_wrapper/types(本目录, 只有全局声明)
 *   <exe>/jz_engine/jz_types/<pkg>/types/**  <- js_wrapper/<pkg>/types(模块类型 + ambient.d.ts)
 *   <exe>/jz_engine/jz_js/<pkg>/dist/**      <- js_wrapper/<pkg>/dist
 *
 * Usage in a TS project:
 *   1. copy the deployed jz_types/ folder to <project>/jz_types/
 *   2. include it in tsconfig.json:
 *        "include": ["src/**\/*.ts", "jz_types/**\*.d.ts"]
 *   3. set "lib": ["ES2022"] (without DOM), otherwise the global WebSocket,
 *      fetch, Response, Headers, URL and URLSearchParams declared here collide
 *      with lib.dom.d.ts and TS reports duplicate identifier errors.
 */
