/// <reference path="common.d.ts" />
/// <reference path="events.d.ts" />
/// <reference path="net.d.ts" />
/// <reference path="dgram.d.ts" />
/// <reference path="http.d.ts" />
/// <reference path="fetch.d.ts" />
/// <reference path="websocket.d.ts" />
/// <reference path="mqtt.d.ts" />
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
 * fs, path, process, crypto) and require() returns the very same object.
 *
 * Types are declared as ambient modules, so use a type only import to get them
 * (it is erased at runtime and never triggers a module load):
 *
 *   import type { Socket } from 'net';
 *   const net = require('net');
 *
 * Usage in a TS project:
 *   1. copy this folder to <project>/types/
 *   2. include it in tsconfig.json:
 *        "include": ["src/**\/*.ts", "types/**\/*.d.ts"]
 *   3. set "lib": ["ES2022"] (without DOM), otherwise the global WebSocket,
 *      fetch, Response, Headers, URL and URLSearchParams declared here collide
 *      with lib.dom.d.ts and TS reports duplicate identifier errors.
 */

declare function require(name: 'net'): typeof import('net');
declare function require(name: 'dgram'): typeof import('dgram');
declare function require(name: 'http'): typeof import('http');
declare function require(name: 'mqtt'): typeof import('mqtt');
declare function require(name: 'url'): typeof import('url');
declare function require(name: 'os'): typeof import('os');
declare function require(name: 'ws'): typeof import('ws');
declare function require(name: 'fs'): typeof import('fs');
declare function require(name: 'path'): typeof import('path');
declare function require(name: 'process'): typeof import('process');
declare function require(name: 'crypto'): typeof import('crypto');
declare function require(name: string): any;
