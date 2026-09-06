// JZJSNetModule common types
// All .d.ts in this folder are ambient global declarations (no top level
// import/export): the modules are injected into the global scope by the engine
// at runtime, while the types are exposed as ambient modules so that
// `import type { Socket } from 'net'` works (type only, erased at runtime).

/**
 * Encodings supported when converting between bytes and string.
 * Only a subset of Node BufferEncoding is implemented.
 */
type DataEncoding = 'utf8' | 'utf-8' | 'hex' | 'base64' | 'latin1';

/** Data received from events (string when setEncoding was used) */
type Chunk = Uint8Array | string;

/** Data accepted by write/send */
type Data = Uint8Array | string;

/**
 * quickjs already implements the tc39 arraybuffer-base64 proposal, but the
 * ES2022 lib does not declare it yet, so declare it here.
 */
interface Uint8Array {
    toHex(): string;
    toBase64(options?: { alphabet?: 'base64' | 'base64url' }): string;
    setFromHex(hex: string): { read: number; written: number };
    setFromBase64(base64: string, options?: { alphabet?: 'base64' | 'base64url' }): { read: number; written: number };
}

interface Uint8ArrayConstructor {
    fromHex(hex: string): Uint8Array;
    fromBase64(base64: string, options?: { alphabet?: 'base64' | 'base64url' }): Uint8Array;
}
