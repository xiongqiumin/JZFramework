/**
 * @jz/audio —— 录音 / 放音 / 设备 / WAV / 重采样（走 `__jz.audio` 原子能力，见 JZJSAudioModule）。
 *
 * 形状说明（为什么这么设计）:
 *   · 低层函数与 __jz.audio 一一对应（openInput / readInput / writeOutput / wavWrite / transcode ...），
 *     字节一律 Uint8Array（收 Buffer/ArrayBuffer/字符串也行）。
 *   · 采集在 C++ 侧排队：音频回调只 append，**不碰 JS**（引擎不是线程安全的）。
 *     所以这里用 `read()` 主动取；`onData(cb)` 是**本层**用 setInterval 轮询实现的"事件式"外观。
 *   · 常用整体流程封装成 Promise：`record()`（录一段）/ `play()`（放一段）/ `recordToWav()`。
 *   · 依赖宿主装了 __jz.audio（JZJSAudioModule）：没编 JZAudio 的构建里 available() 为 false，
 *     调用会抛带原因的错（而不是静默失败）。
 */
'use strict';

declare const __jz: any;
declare function setInterval(cb: () => void, ms: number): number;
declare function clearInterval(id: number): void;
declare function setTimeout(cb: () => void, ms: number): number;

/** 采样格式: 交给后端时可以是名字, 也可以是 JZAudioFormat::SampleFormat 的数(0..3) */
export type SampleFormat = 'uint8' | 'int16' | 'int32' | 'float';

/** 打开设备时给的格式（默认 16000Hz / 1 声道 / int16，与 JZAudioFormat 的默认一致） */
export interface AudioFormat {
    sampleRate?: number;
    channelCount?: number;
    sampleFormat?: SampleFormat | number;
    /** 设备 id（来自 devices()；不给就用默认设备） */
    deviceId?: string;
}

/** 后端回报的实际格式（多带了名字与字节数） */
export interface AudioFormatInfo {
    channelCount: number;
    sampleRate: number;
    sampleFormat: number;
    sampleFormatName: string;
    bytesPerSample: number;
    /** 一帧(所有通道)占多少字节 = bytesPerSample * channelCount */
    frameBytes: number;
}

export interface AudioDevice { id: string; name: string; isInput: boolean; }
export interface AudioDevices { inputs: AudioDevice[]; outputs: AudioDevice[]; }

export interface AudioInputInfo extends AudioFormatInfo {
    /** 后端还缓冲着多少字节（没被 read 取走） */
    buffered: number;
    /** 缓冲到上限(8MB)丢过数据 */
    overrun: boolean;
}

export interface WavData { format: AudioFormatInfo; frames: number; data: Uint8Array; }

// ————————————————————————————————————————————————————————————
// 低层（与 __jz.audio 一一对应）
// ————————————————————————————————————————————————————————————

function backend(): any {
    if (!__jz || !__jz.audio)
        throw new Error('[audio] 宿主没装 __jz.audio（JZJSAudioModule）—— 模块 import 得到，但调不了');
    return __jz.audio;
}

function check(v: any, what: string): any {
    if (v && typeof v === 'object' && v.error)
        throw new Error('[audio] ' + what + ' 失败: ' + v.error);
    return v;
}

/** 这个构建装没装音频（没编 JZAudio 时为 false，别处调用会抛错） */
export function available(): boolean {
    return !!(__jz && __jz.audio && __jz.audio.available());
}

/** 列设备：{inputs:[{id,name}], outputs:[{id,name}]} */
export function devices(): AudioDevices {
    const d = check(backend().devices(), 'devices');
    return { inputs: d.inputs || [], outputs: d.outputs || [] };
}

/** 开一个输入（录音）通道，回句柄；打不开返回 null */
export function openInput(opts: AudioFormat = {}): AudioInput | null {
    const id = backend().openInput(opts);
    return (id > 0) ? new AudioInput(id) : null;
}

/** 开一个输出（放音）通道，回句柄；打不开返回 null */
export function openOutput(opts: AudioFormat = {}): AudioOutput | null {
    const id = backend().openOutput(opts);
    return (id > 0) ? new AudioOutput(id) : null;
}

/** 读 WAV 头（不读数据）—— 配合 wavReadAll 用；wavRead 是它的合并版 */
export function wavInfo(path: string): AudioFormatInfo & { dataSize: number; frames: number } {
    return check(backend().wavInfo(path), 'wavInfo');
}

/** 读 WAV 的整段 PCM（字节就是文件里的 PCM，不做格式转换） */
export function wavReadAll(path: string): Uint8Array {
    return backend().wavReadAll(path);
}

/** 读 WAV：头 + 数据一起给 */
export function wavRead(path: string): WavData {
    const info = wavInfo(path);
    return { format: info, frames: info.frames, data: wavReadAll(path) };
}

/** 写 WAV（按给定格式把 PCM 原样写成 wav 文件，头的大小字段由后端维护） */
export function wavWrite(path: string, format: AudioFormat, data: Uint8Array): boolean {
    return !!backend().wavWrite(path, format, data);
}

/** 重采样 / 声道转换（后端 audioTrans：线性插值 + 单声道复制；⚠ 内部按 int16 处理） */
export function transcode(data: Uint8Array, inFormat: AudioFormat, outFormat: AudioFormat): Uint8Array {
    return backend().transcode(data, inFormat, outFormat);
}

/** 关掉所有打开的输入/输出（返回关掉的数量）—— deinitEngine 时也会自动做 */
export function releaseAll(): number {
    return __jz && __jz.audio ? __jz.audio.releaseAll() : 0;
}

// ————————————————————————————————————————————————————————————
// 输入句柄（录音）
// ————————————————————————————————————————————————————————————

export class AudioInput {
    readonly id: number;
    private timer: number | null = null;
    private cbs: ((chunk: Uint8Array) => void)[] = [];

    /** @internal 用 openInput() 拿，别自己 new */
    constructor(id: number) { this.id = id; }

    get info(): AudioInputInfo { return check(backend().inputInfo(this.id), 'inputInfo'); }

    /** 取空当前已缓冲的数据（没有数据就是长度 0 的 Uint8Array） */
    read(): Uint8Array { return backend().readInput(this.id); }

    /**
     * 事件式收数据：每 `interval` ms 拉一次，有数据才回调。
     * ⚠ 这是**本层**用定时器做的轮询（C++ 侧不能在音频线程里直接调 JS）。
     */
    onData(cb: (chunk: Uint8Array) => void, interval = 50): AudioInput {
        this.cbs.push(cb);
        if (this.timer === null) {
            this.timer = setInterval(() => {
                const chunk = this.read();
                if (chunk.length === 0) return;
                for (let i = 0; i < this.cbs.length; i++) this.cbs[i](chunk);
            }, interval);
        }
        return this;
    }

    /** 撤回调（不传就把全部撤掉）；全撤掉会停掉轮询定时器 */
    offData(cb?: (chunk: Uint8Array) => void): AudioInput {
        if (!cb) this.cbs = [];
        else this.cbs = this.cbs.filter((f) => f !== cb);
        if (this.cbs.length === 0 && this.timer !== null) {
            clearInterval(this.timer);
            this.timer = null;
        }
        return this;
    }

    /** 关掉这个通道（同时撤掉回调与定时器） */
    close(): boolean {
        this.offData();
        return !!backend().closeInput(this.id);
    }
}

// ————————————————————————————————————————————————————————————
// 输出句柄（放音）
// ————————————————————————————————————————————————————————————

export class AudioOutput {
    readonly id: number;

    /** @internal 用 openOutput() 拿，别自己 new */
    constructor(id: number) { this.id = id; }

    /** 流式喂 PCM（队列满会返回 false，调用方稍后重试） */
    write(data: Uint8Array): boolean { return !!backend().writeOutput(this.id, data); }

    /** 上一次播完没有（读一次就清） */
    get finished(): boolean { return !!backend().outputFinished(this.id); }

    close(): boolean { return !!backend().closeOutput(this.id); }
}

// ————————————————————————————————————————————————————————————
// 整体流程（Promise）
// ————————————————————————————————————————————————————————————

export interface RecordOptions extends AudioFormat {
    /** 录多久（毫秒；默认 1000）。也可以不传, 用 stop() 手动停。 */
    ms?: number;
    /** 边录边拿数据（可选） */
    onChunk?: (chunk: Uint8Array) => void;
    /** 轮询间隔（毫秒，默认 20）—— 越小越实时，越大越省 */
    interval?: number;
}

/** 录一段，回 PCM（默认 int16/16k/单声道，1 秒） */
export function record(opts: RecordOptions = {}): Promise<Uint8Array> {
    return new Promise<Uint8Array>((resolve, reject) => {
        const input = openInput(opts);
        if (!input)
            return reject(new Error('[audio] 打不开输入设备（可用 devices() 看设备列表）'));
        const chunks: Uint8Array[] = [];
        let total = 0;
        const keep = (c: Uint8Array) => {
            chunks.push(c);
            total += c.length;
            if (opts.onChunk) opts.onChunk(c);
        };
        const interval = opts.interval || 20;
        const timer = setInterval(() => {
            const c = input.read();
            if (c.length > 0) keep(c);
        }, interval);
        setTimeout(() => {
            clearInterval(timer);
            const tail = input.read();
            if (tail.length > 0) keep(tail);
            input.close();
            const out = new Uint8Array(total);
            let off = 0;
            for (let i = 0; i < chunks.length; i++) { out.set(chunks[i], off); off += chunks[i].length; }
            resolve(out);
        }, opts.ms === undefined ? 1000 : opts.ms);
    });
}

/** 录一段并直接存成 WAV，回写入后的信息（含实际帧数） */
export function recordToWav(path: string, opts: RecordOptions = {}): Promise<WavData> {
    return record(opts).then((data) => {
        const format: AudioFormat = {
            sampleRate: opts.sampleRate || 16000,
            channelCount: opts.channelCount || 1,
            sampleFormat: opts.sampleFormat === undefined ? 'int16' : opts.sampleFormat,
        };
        if (!wavWrite(path, format, data))
            throw new Error('[audio] 写 WAV 失败: ' + path);
        const info = wavInfo(path);
        return { format: info, frames: info.frames, data };
    });
}

export interface PlayOptions extends AudioFormat {
    /** 每次喂多少字节（默认 4096，与后端缓冲块同量级） */
    chunkBytes?: number;
    /** 队列满时的重试间隔（毫秒，默认 20） */
    retryMs?: number;
}

/** 放一段 PCM，播完了才 resolve（内部：分块喂 + 队列满重试 + 等播完信号） */
export function play(data: Uint8Array, opts: PlayOptions = {}): Promise<void> {
    return new Promise<void>((resolve, reject) => {
        const out = openOutput(opts);
        if (!out)
            return reject(new Error('[audio] 打不开输出设备（可用 devices() 看设备列表）'));
        const chunk = opts.chunkBytes || 4096;
        const retry = opts.retryMs || 20;
        let off = 0;
        const pump = () => {
            while (off < data.length) {
                const end = Math.min(off + chunk, data.length);
                if (!out.write(data.subarray(off, end))) {
                    setTimeout(pump, retry);        // 后端的播放队列满了(4 块): 稍后再喂
                    return;
                }
                off = end;
            }
            const wait = () => {
                if (out.finished) { out.close(); resolve(); return; }
                setTimeout(wait, 30);
            };
            wait();
        };
        pump();
    });
}

// ————————————————————————————————————————————————————————————
// 小工具（生成/分析 PCM，都是 int16 视角）
// ————————————————————————————————————————————————————————————

export interface ToneOptions {
    seconds?: number;
    freq?: number;
    sampleRate?: number;
    /** 幅值 0..32767（默认 12000，够响但不爆） */
    amplitude?: number;
}

/** 生成一段正弦音（int16 PCM）—— 提示音 / 测试用 */
export function makeTone(opts: ToneOptions = {}): Uint8Array {
    const seconds = opts.seconds === undefined ? 0.5 : opts.seconds;
    const freq = opts.freq === undefined ? 440 : opts.freq;
    const rate = opts.sampleRate === undefined ? 16000 : opts.sampleRate;
    const amp = opts.amplitude === undefined ? 12000 : opts.amplitude;
    const n = Math.max(1, Math.round(seconds * rate));
    const out = new Uint8Array(n * 2);
    for (let i = 0; i < n; i++) {
        const v = Math.round(Math.sin(2 * Math.PI * freq * i / rate) * amp);
        out[i * 2] = v & 0xff;
        out[i * 2 + 1] = (v >> 8) & 0xff;
    }
    return out;
}

/** int16 PCM -> -1..1 的 Float32（做频谱/过零率之类的分析用） */
export function pcm16ToFloat32(data: Uint8Array): Float32Array {
    const n = data.length >> 1;
    const out = new Float32Array(n);
    for (let i = 0; i < n; i++) {
        let v = data[i * 2] | (data[i * 2 + 1] << 8);
        if (v > 32767) v -= 65536;
        out[i] = v / 32768;
    }
    return out;
}

/** 这段 PCM 的峰值（0..1）—— 判"有没有声音"最省事 */
export function peak(data: Uint8Array): number {
    let p = 0;
    for (let i = 0; i + 1 < data.length; i += 2) {
        let v = data[i] | (data[i + 1] << 8);
        if (v > 32767) v -= 65536;
        const a = v < 0 ? -v : v;
        if (a > p) p = a;
    }
    return p / 32768;
}

/** 这段 PCM 的均方根（0..1）—— 比峰值稳，做静音判定好用 */
export function rms(data: Uint8Array): number {
    let sum = 0;
    let n = 0;
    for (let i = 0; i + 1 < data.length; i += 2) {
        let v = data[i] | (data[i + 1] << 8);
        if (v > 32767) v -= 65536;
        sum += v * v;
        n++;
    }
    return n > 0 ? Math.sqrt(sum / n) / 32768 : 0;
}

const audio = {
    available, devices, openInput, openOutput, AudioInput, AudioOutput,
    record, recordToWav, play, makeTone, pcm16ToFloat32, peak, rms,
    wavInfo, wavReadAll, wavRead, wavWrite, transcode, releaseAll,
};
export default audio;
