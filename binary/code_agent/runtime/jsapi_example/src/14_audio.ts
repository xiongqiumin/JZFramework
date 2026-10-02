/**
 * 14_audio —— 音频包 `@jz/audio`（录音 / 放音 / 设备 / WAV / 重采样）。
 *
 * 跑法（与其它例子相同）:
 *   <build>/binary/code_agent/bin/<CONFIG>/code_runtime.exe --entry <build>/jsapi_example/build/14_audio.js
 *   ctest -C <CONFIG> -R jsapi_example.14_audio
 *
 * 设备无关的部分（WAV 往返 / 重采样 / 波形工具）无条件断言; 真录/真放要看机器有没有设备,
 * 没有就打印一行跳过 —— 这样 CI/无声卡机器上也是绿的（异步断言失败走"未处理 promise -> 退出码 1"）。
 */
import audio from 'audio';
import { makeTone, peak, rms, pcm16ToFloat32 } from 'audio';
import { check } from './helper/assert.js';

// ---- 1. 能力探测 + 设备枚举 ----
const ok = audio.available();
console.log('audio.available() = ' + ok);
check('available() 回 boolean', typeof ok === 'boolean');
check('本构建编入了 JZAudio(HAS_JZAudio=ON)', ok === true);

const devs = audio.devices();
check('devices() 回 {inputs, outputs}', Array.isArray(devs.inputs) && Array.isArray(devs.outputs));
console.log('设备: 输入 ' + devs.inputs.length + ' 个, 输出 ' + devs.outputs.length + ' 个');
if (devs.inputs.length > 0)
    console.log('  输入[0] = ' + devs.inputs[0].name + ' (id=' + devs.inputs[0].id + ')');
if (devs.outputs.length > 0) {
    console.log('  输出[0] = ' + devs.outputs[0].name + ' (id=' + devs.outputs[0].id + ')');
    check('输出设备的 isInput 为 false', devs.outputs[0].isInput === false);
}

// ---- 2. 波形工具（设备无关） ----
const tone = makeTone({ seconds: 0.25, freq: 880, sampleRate: 16000 });
console.log('makeTone: ' + tone.length + ' 字节');
check('makeTone 生成 16k/0.25s/int16 = 8000 字节', tone.length === 8000);
const pk = peak(tone);
check('峰值接近设定幅值(0.3~0.4), 实测 ' + pk.toFixed(3), pk > 0.3 && pk < 0.4);
const rm = rms(tone);
check('正弦 rms ≈ 峰值/√2, 实测 ' + rm.toFixed(3), rm > 0.2 && rm < 0.3);
const f32 = pcm16ToFloat32(tone);
check('pcm16ToFloat32 长度 = 样本数(4000), 实测 ' + f32.length, f32.length === 4000);
check('float 值域在 ±1 内', f32[100] > -1 && f32[100] < 1);

// ---- 3. WAV 写 -> 读 往返（设备无关） ----
const dir = (process.cwd ? process.cwd() : '.');
const wavPath = dir + '/14_audio_tone.wav';
const fmt = { sampleRate: 16000, channelCount: 1, sampleFormat: 'int16' } as const;
check('wavWrite 成功', audio.wavWrite(wavPath, fmt, tone) === true);

const info = audio.wavInfo(wavPath);
console.log('wavInfo: ' + info.sampleRate + 'Hz/' + info.channelCount + 'ch/' + info.sampleFormatName
    + ', ' + info.frames + ' 帧, ' + info.dataSize + ' 字节');
check('wavInfo 帧数 = 4000', info.frames === 4000);
check('wavInfo 采样格式 = int16', info.sampleFormatName === 'int16');

const back = audio.wavRead(wavPath);
let same = back.data.length === tone.length;
for (let i = 0; same && i < tone.length; i++)
    if (back.data[i] !== tone[i]) same = false;
check('读回来的 PCM 与写入的逐字节一致(' + back.data.length + ' 字节)', same);

// ---- 4. 重采样 / 声道转换（设备无关） ----
const half = audio.transcode(tone, { sampleRate: 16000 }, { sampleRate: 8000 });
check('16k -> 8k 字节数减半(实测 ' + half.length + ')', half.length === tone.length / 2);
const stereo = audio.transcode(tone, { channelCount: 1 }, { channelCount: 2 });
check('单声道 -> 双声道 字节数翻倍(实测 ' + stereo.length + ')', stereo.length === tone.length * 2);

// ---- 5. 真录 / 真放（有设备才做） ----
const jobs: Promise<any>[] = [];

if (devs.outputs.length > 0) {
    jobs.push(audio.play(tone, fmt).then(() => {
        console.log('[PASS] play(): 0.25s 的 880Hz 放完了');
    }));
} else {
    console.log('没有输出设备, 跳过播放测试');
}

if (devs.inputs.length > 0) {
    jobs.push(audio.record({ ms: 400, sampleRate: 16000, channelCount: 1 }).then((pcm: Uint8Array) => {
        console.log('record(): 400ms 录到 ' + pcm.length + ' 字节, 峰值 ' + peak(pcm).toFixed(3));
        check('录到的字节数与时长同量级(实测 ' + pcm.length + ')', pcm.length > 4000);
        check('能直接存成 WAV', audio.wavWrite(dir + '/14_audio_record.wav', fmt, pcm) === true);
    }));
} else {
    console.log('没有输入设备, 跳过录音测试');
}

Promise.all(jobs).then(() => {
    audio.releaseAll();
    console.log('14_audio done');
}).catch((e) => {
    throw e;        // 丢成未处理 promise -> 引擎上报 -> 退出码 1
});
