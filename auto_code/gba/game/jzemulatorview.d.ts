// JZEmulatorView 的**任务内**类型声明。
//
// 为什么不在 JZJSEngine/js_wrapper/widget/types 里：JZEmulator 是**单例模块**
// （独立静态库，见 JZEmulator/JZEmulatorViewJs.h 的"依赖方向"说明）——
// 引擎没接这个模块时 JS 里 `new JZEmulatorView()` 会抛"引擎没有注册
// init_c_emulator_view"。所以它是"可选类"，类型声明跟着用它的地方走。
//
// 方法集与 JZEmulator/JZEmulatorViewJs.cpp 的 bind.def(...) 一一对应。
/** @final */
declare class JZEmulatorView extends JZWidget {
  constructor(parent?: JZWidget | null);

  /** 加载宿主 DLL（dllDir 里找 mgba_host.dll）+ 打开 ROM（.gba/.gb/.gbc）。失败原因见 lastError() */
  openRom(dllDir: string, romPath: string): boolean;
  isRomOpen(): boolean;
  lastError(): string;

  /** 开跑：每拍（16ms ≈ 60Hz）同步按键 → 跑一帧 → 换帧。⚠ JS 绑定是 0 参数（节拍不可从 JS 改） */
  start(): void;
  stop(): void;
  isRunning(): boolean;

  /** 已经跑过多少帧（定时器真在跑就会涨） */
  frameCount(): number;
}
