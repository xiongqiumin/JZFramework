// modbus 包对外暴露的 Node 风格模块名 (ambient 模块声明): modbus。
//
// 与 core/net/serialport/sqlite 的 ambient.d.ts 同理: `declare module '<裸名>'` 只能写在
// **全局脚本文件**里(无顶层 import/export), 所以单独一个文件, 只声明裸名并转发给
// 同目录的模块类型 ./index.d.ts(单一来源)。
//
// 形态与 modbus-serial 一致 = 有默认导出(默认导出就是 ModbusRTU 类):
//   import ModbusRTU from 'modbus';               // 对
//   import { ModbusRTU, ModbusTCP } from 'modbus';// 也可以(具名导出都在)
// ⚠ 少这个文件时 `import ... from 'modbus'` 报 TS2307, 运行期其实是好的。
declare module 'modbus' {
    export * from './index';
    export { default } from './index';
}
