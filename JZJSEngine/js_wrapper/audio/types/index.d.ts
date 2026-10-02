/// <reference path="../../types/common.d.ts" />

/* @jz/audio 的类型面 —— 与 index.ts 的实现一一对应（消费者只用这份，不看 dist）。 */

export type SampleFormat = 'uint8' | 'int16' | 'int32' | 'float';

export interface AudioFormat {
    sampleRate?: number;
    channelCount?: number;
    sampleFormat?: SampleFormat | number;
    deviceId?: string;
}

export interface AudioFormatInfo {
    channelCount: number;
    sampleRate: number;
    sampleFormat: number;
    sampleFormatName: string;
    bytesPerSample: number;
    frameBytes: number;
}

export interface AudioDevice { id: string; name: string; isInput: boolean; }
export interface AudioDevices { inputs: AudioDevice[]; outputs: AudioDevice[]; }

export interface AudioInputInfo extends AudioFormatInfo {
    buffered: number;
    overrun: boolean;
}

export interface WavData { format: AudioFormatInfo; frames: number; data: Uint8Array; }

/** 这个构建装没装音频（没编 JZAudio 时为 false） */
export function available(): boolean;

/** 列设备 */
export function devices(): AudioDevices;

export class AudioInput {
    readonly id: number;
    readonly info: AudioInputInfo;
    /** 取空当前缓冲的数据（没有就是长度 0） */
    read(): Uint8Array;
    /** 事件式收数据（本层用定时器轮询实现） */
    onData(cb: (chunk: Uint8Array) => void, interval?: number): AudioInput;
    offData(cb?: (chunk: Uint8Array) => void): AudioInput;
    close(): boolean;
}

export class AudioOutput {
    readonly id: number;
    /** 流式喂 PCM（队列满返回 false，稍后重试） */
    write(data: Uint8Array): boolean;
    /** 上一次播完没有（读一次就清） */
    readonly finished: boolean;
    close(): boolean;
}

export function openInput(opts?: AudioFormat): AudioInput | null;
export function openOutput(opts?: AudioFormat): AudioOutput | null;

export interface RecordOptions extends AudioFormat {
    ms?: number;
    onChunk?: (chunk: Uint8Array) => void;
    interval?: number;
}

/** 录一段（默认 int16/16k/单声道/1 秒） */
export function record(opts?: RecordOptions): Promise<Uint8Array>;
export function recordToWav(path: string, opts?: RecordOptions): Promise<WavData>;

export interface PlayOptions extends AudioFormat {
    chunkBytes?: number;
    retryMs?: number;
}

/** 放一段 PCM，播完才 resolve */
export function play(data: Uint8Array, opts?: PlayOptions): Promise<void>;

export function wavInfo(path: string): AudioFormatInfo & { dataSize: number; frames: number };
export function wavReadAll(path: string): Uint8Array;
export function wavRead(path: string): WavData;
export function wavWrite(path: string, format: AudioFormat, data: Uint8Array): boolean;

/** 重采样 / 声道转换（⚠ 后端按 int16 处理） */
export function transcode(data: Uint8Array, inFormat: AudioFormat, outFormat: AudioFormat): Uint8Array;

/** 关掉所有打开的输入/输出，返回关掉的数量 */
export function releaseAll(): number;

export interface ToneOptions { seconds?: number; freq?: number; sampleRate?: number; amplitude?: number; }

/** 生成一段正弦（int16 PCM） */
export function makeTone(opts?: ToneOptions): Uint8Array;
/** int16 PCM -> -1..1 Float32 */
export function pcm16ToFloat32(data: Uint8Array): Float32Array;
/** 峰值（0..1） */
export function peak(data: Uint8Array): number;
/** 均方根（0..1） */
export function rms(data: Uint8Array): number;

declare const _default: {
    available: typeof available, devices: typeof devices,
    openInput: typeof openInput, openOutput: typeof openOutput,
    AudioInput: typeof AudioInput, AudioOutput: typeof AudioOutput,
    record: typeof record, recordToWav: typeof recordToWav, play: typeof play,
    makeTone: typeof makeTone, pcm16ToFloat32: typeof pcm16ToFloat32,
    peak: typeof peak, rms: typeof rms,
    wavInfo: typeof wavInfo, wavReadAll: typeof wavReadAll, wavRead: typeof wavRead,
    wavWrite: typeof wavWrite, transcode: typeof transcode, releaseAll: typeof releaseAll,
};
export default _default;
