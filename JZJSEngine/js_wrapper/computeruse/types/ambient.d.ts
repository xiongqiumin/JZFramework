// 'computeruse' —— 对外的裸模块名(= 目录名): 这里只声明名字, 类型转发给同目录的 ./index.d.ts
// (ambient `declare module` 只能写在全局脚本文件里, 所以单独一个文件)。
//
// ⚠ TS7 + moduleResolution:bundler 下, ambient 内 `export * from './index'` 不会转发具名导出
//   (只转发 default), 故这里改**显式具名再导出**(与 buffer 包同一个坑, 同一个写法)。
//
// 注意本包的特殊之处:
//   - 包入口只有具名导出(**没有 default**) ⇒ 正确写法是
//       import { App, setBackend } from 'computeruse';
//     运行期靠 builtinModules 的 namespace 形态('*')桥接, 不合成 default。
//   - 真正的"操作系统"动作(点击/按键/截图/查找) 不在本包: 由 __jz.computeruse.* 提供
//     (C++ JZJSComputerUseModule), 本包只管控件树与 UI 操作, 并留 setBackend 注入点
//     (离线/模拟环境或测试夹具用它替换后端)。
declare module 'computeruse' {
    export type {
        Rect, Point, MouseBtn, Presence, Locator, ExploreOptions, ModelSnapshot, NodeOpts,
        Perception, PerceptionElement, VlmOptions,
        ComputerUseBackend,
        ICuNode, IButton, IMenu, ITab, IToolbar, IGroup, IWindow, ILabel,
        ICanvas, IEdit, IComboBox, ICheckBox, ISlider, ILink, IApp,
    } from './index';
    export {
        backend, setBackend,
        CuNode, Button, Link, Tab, Menu, Toolbar, Group, Window, Label,
        Canvas, Edit, ComboBox, CheckBox, Slider, App,
        key, type, wait,
        perceive, buildPrompt, setVlmConfig, vlmConfig,
    } from './index';
}
