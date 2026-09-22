export function cwd(): string;
export const execPath: string;
export const platform: string;
export const argv: string[];

/** default export = the module object (same usage as Node: import x from 'process') */
declare const _default: { cwd: typeof cwd, execPath: typeof execPath, platform: typeof platform, argv: typeof argv };
export default _default;
