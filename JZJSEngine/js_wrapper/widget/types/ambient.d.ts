// 'widget' —— 对外的裸模块名(= 目录名): 这里只声明名字, 类型转发给同目录的 ./index.d.ts
// (ambient `declare module` 只能写在全局脚本文件里, 所以单独一个文件)。
//
// 注意本包的特殊之处:
//   - 控件类是**引擎注册的全局类**(JZWindow / JZPushButton / ...), 运行期由 C++ 按路径
//     先加载 widget/dist/classes.js 与 widget/dist/index.js, 裸名 import 只是"同样能拿到"。
//   - 包入口只有具名导出(没有 default) ⇒ `import { JZWindow } from 'widget'` 是正确写法。
declare module 'widget' {
    export * from './index';
}
