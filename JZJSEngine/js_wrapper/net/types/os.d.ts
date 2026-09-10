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
