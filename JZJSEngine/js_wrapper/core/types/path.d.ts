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
