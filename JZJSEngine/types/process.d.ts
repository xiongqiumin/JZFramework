/// <reference path="common.d.ts" />

declare module 'process' {
    export function cwd(): string;
    export const execPath: string;
    export const platform: string;
    export const argv: string[];
}

/** the engine injects the same object into the global scope */
declare const process: typeof import('process');
