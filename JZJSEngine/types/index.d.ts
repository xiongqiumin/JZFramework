/// <reference path="common.d.ts" />
/// <reference path="events.d.ts" />
/// <reference path="globals.d.ts" />
/// <reference path="console.d.ts" />
/// <reference path="fetch.d.ts" />

/// <reference path="serialport.d.ts" />
/// <reference path="net.d.ts" />
/// <reference path="dgram.d.ts" />
/// <reference path="http.d.ts" />
/// <reference path="mqtt.d.ts" />
/// <reference path="websocket.d.ts" />
/// <reference path="url.d.ts" />
/// <reference path="os.d.ts" />
/// <reference path="fs.d.ts" />
/// <reference path="path.d.ts" />
/// <reference path="process.d.ts" />
/// <reference path="crypto.d.ts" />

/**
 * JZJSNetModule / JZJSCoreModule type definitions.
 *
 * The modules are injected into the global scope by the engine, so every module
 * is also available as a global value (net, dgram, http, mqtt, url, os, ws,
 * fs, path, process, crypto, serialport).
 *
 * Types are declared as ambient modules, so use a type only import to get them
 * (it is erased at runtime and never triggers a module load):
 *
 *   import type { Socket } from 'net';
 *
 * Runtime usage goes through ES module import from the compiled package path
 * (require() is NOT supported):
 *
 *   import { net } from './net/dist/index.js';
 *   import { fs } from './core/dist/index.js';
 *   import { SerialPort } from './serialport/dist/index.js';
 *
 * The ambient modules above forward to the per-module packages under
 * JZJSEngine/js_wrapper/<module>/types/index.d.ts (single source of truth).
 *
 * Usage in a TS project:
 *   1. copy this folder to <project>/types/
 *   2. include it in tsconfig.json:
 *        "include": ["src/**\/*.ts", "types/**\*.d.ts"]
 *   3. set "lib": ["ES2022"] (without DOM), otherwise the global WebSocket,
 *      fetch, Response, Headers, URL and URLSearchParams declared here collide
 *      with lib.dom.d.ts and TS reports duplicate identifier errors.
 */
