// 'computeruse' —— 对外的裸模块名(= 目录名): 这里只声明名字, 类型转发给同目录的 ./index.d.ts
// (ambient `declare module` 只能写在全局脚本文件里, 所以单独一个文件)。
//
// ⚠ TS7 + moduleResolution:bundler 下, ambient 内 `export * from './index'` 不会转发具名导出
//   (只转发 default), 故这里改**显式具名再导出**(与 buffer 包同一个坑, 同一个写法)。
//   ⛔ 加新导出时必须**同时**在下面两张表里补名, 否则 `import { X }` 报 TS2305。
//
// 注意本包的特殊之处:
//   - 包入口只有具名导出(**没有 default**) ⇒ 正确写法是
//       import { App, setBackend, diagnose } from 'computeruse';
//     运行期靠 builtinModules 的 namespace 形态('*')桥接, 不合成 default。
//   - 真正的"操作系统"动作(点击/按键/截图/查找) 不在本包: 由 __jz.computeruse.* 提供
//     (C++ JZJSComputerUseModule), 本包只管控件树与 UI 操作, 并留 setBackend 注入点
//     (离线/模拟环境或测试夹具用它替换后端)。
declare module 'computeruse' {
    export type {
        Rect, Point, MouseBtn, Presence, Locator,
        // ⭐ 布局通道 / 运行期寻址 / 本地分级
        LayoutPath, LayoutNode, PageFingerprint, AttachResult, NodeState, ExpectMatcher,
        SyncOptions, LaunchOptions, FailureKind, FailureAction, DiagnoseContext, Diagnosis,
        CuErrorCode, FlatEntry, EvidencePack,
        ExploreOptions, ModelSnapshot, NodeOpts, PerceptionReview,
        Perception, PerceptionElement, VlmOptions,
        ComputerUseBackend,
        ICuNode, IButton, IMenu, ITab, IToolbar, IGroup, IWindow, ILabel,
        ICanvas, IEdit, IComboBox, ICheckBox, ISlider, ILink, IApp,
    } from './index';
    export {
        backend, setBackend, CuError,
        CuNode, Button, Link, Tab, Menu, Toolbar, Group, Window, Label,
        Canvas, Edit, ComboBox, CheckBox, Slider, App,
        key, type, wait,
        perceive, buildPrompt, setVlmConfig, vlmConfig,
        flatten, nodeAt, parentPath, fingerprintOf, fingerprintMatch, samePage, extraTopPaths,
        claimLayout, layoutCoverRatio,
        diagnose, formatDiagnosis, formatEvidence, stateMatches, explainMismatch,
    } from './index';
}
