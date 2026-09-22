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

/** default export = the module object (same usage as Node: import x from 'os') */
declare const _default: { networkInterfaces: typeof networkInterfaces, platform: typeof platform, tmpdir: typeof tmpdir, EOL: typeof EOL };
export default _default;
