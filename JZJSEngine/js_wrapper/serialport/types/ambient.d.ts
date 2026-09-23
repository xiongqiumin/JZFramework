// 'serialport' —— 对外的裸模块名(= 目录名): 类型转发给同目录的 ./index.d.ts。
// ⚠ TS7 + moduleResolution:bundler 下, ambient 内 `export * from './index'` 不会转发具名导出
//   (只转发 default), 故这里改显式具名再导出(等价且能编译通过)。
declare module 'serialport' {
    export { Data, EventEmitter, OpenOptions, PortInfo, SerialPort, list, serialport } from './index';
    export { default } from './index';
}
