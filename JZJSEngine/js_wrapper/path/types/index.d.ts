interface ParsedPath {
    root: string;
    dir: string;
    base: string;
    ext: string;
    name: string;
}

export const sep: string;
export const delimiter: string;
export function basename(p: string, ext?: string): string;
export function dirname(p: string): string;
export function extname(p: string): string;
export function isAbsolute(p: string): boolean;
export function join(...paths: string[]): string;
export function normalize(p: string): string;
export function resolve(...paths: string[]): string;
export function parse(p: string): ParsedPath;
export function format(p: ParsedPath): string;
export function relative(from: string, to: string): string;

/** default export = the module object (same usage as Node: import x from 'path') */
declare const _default: { sep: typeof sep, delimiter: typeof delimiter, basename: typeof basename, dirname: typeof dirname, extname: typeof extname, isAbsolute: typeof isAbsolute, join: typeof join, normalize: typeof normalize, resolve: typeof resolve, parse: typeof parse, format: typeof format, relative: typeof relative };
export default _default;
