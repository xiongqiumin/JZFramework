/**
 * @jz/net —— 外部定义汇总 (external definitions, public contract).
 *
 * 本包对应 JZJSNetModule 注入的全局模块: net / dgram / http / mqtt / ws / url / os.
 * 各模块的具名导出分散在同目录的 <module>.d.ts, 这里统一再导出,
 * 便于按包路径引用(如 `import type { Socket } from '@jz/net'`)。
 *
 * 全局类 URL / URLSearchParams / WebSocket 见 JZJSEngine/types/globals.d.ts。
 */
export * from './net';
export * from './dgram';
export * from './http';
export * from './mqtt';
export * from './websocket';
export * from './url';
export * from './os';
