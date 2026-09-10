// 外部定义已统一到 js_wrapper/serialport/types/index.d.ts(由 tsc 编译时与内部实现配套)。
// 这里以 ambient 模块形式转发, 使 import('serialport') / require('serialport') 有类型。
declare module 'serialport' {
    export * from '../js_wrapper/serialport/types/index';
}
declare const serialport: typeof import('serialport');
