// 'audio' —— 对外的裸模块名(= 目录名): 类型转发给同目录的 ./index.d.ts。
// ⚠ TS7 + moduleResolution:bundler 下, ambient 内 `export * from './index'` 不会转发具名导出
//   (只转发 default), 故这里显式具名再导出(与 crypto 包同款)。
declare module 'audio' {
    export {
        SampleFormat, AudioFormat, AudioFormatInfo, AudioDevice, AudioDevices, AudioInputInfo, WavData,
        RecordOptions, PlayOptions, ToneOptions,
        available, devices, openInput, openOutput, AudioInput, AudioOutput,
        record, recordToWav, play, makeTone, pcm16ToFloat32, peak, rms,
        wavInfo, wavReadAll, wavRead, wavWrite, transcode, releaseAll,
    } from './index';
    export { default } from './index';
}
