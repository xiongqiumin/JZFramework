/// <reference path="common.d.ts" />

/**
 * os - only networkInterfaces() is provided (aligned with the Node.js
 * built-in "os" module). Everything else of Node's os module is out of scope.
 */
declare module 'os' {
    export interface NetworkInterfaceInfo {
        address: string;
        family: 'IPv4';
        /** true for 127.* */
        internal: boolean;
        /** netmask / mac / cidr are not available in JZNet */
    }

    export interface NetworkInterfaces {
        [interfaceName: string]: NetworkInterfaceInfo[];
    }

    export function networkInterfaces(): NetworkInterfaces;
}

/** the engine injects the same object into the global scope */
declare const os: typeof import('os');
