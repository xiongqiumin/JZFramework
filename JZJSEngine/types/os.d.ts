/// <reference path="common.d.ts" />

/**
 * os - aligned with the Node.js built-in "os" module. Only the functions that
 * JZCore can back are provided; everything else of Node's os module is out of scope.
 * networkInterfaces() is contributed by the net module (the engine augments this
 * same global object), platform()/tmpdir()/EOL come from the core module.
 */
declare module 'os' {
    export interface NetworkInterfaceInfo {
        address: string;
        family: 'IPv4';
        /** true for 127.* */
        internal: boolean;
    }

    export interface NetworkInterfaces {
        [interfaceName: string]: NetworkInterfaceInfo[];
    }

    export function networkInterfaces(): NetworkInterfaces;
    /** mapped from JZPlatform::platform(): "windows" -> "win32" */
    export function platform(): string;
    export function tmpdir(): string;
    /** "\r\n" on win32, "\n" otherwise */
    export const EOL: string;
}

/** the engine injects the same object into the global scope */
declare const os: typeof import('os');
