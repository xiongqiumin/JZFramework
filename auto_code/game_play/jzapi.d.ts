// play.ts 用到的**引擎注入面**的类型声明（**模板**：驱动每次把它拷进工作区的 `play/`）。
//
// 单一来源（改这两处要一起改）：
//   JZJSEngine/JZJSComputerUseModule.cpp 的 registFunction 列表
//   JZComputerUse/JZComputerUse.h 的语义（归一化坐标 / 键名是 Win32 键名）
//
// ⚠ 这份声明刻意**窄**：脚本能用的就是 computer use 这一套（看画面 + 操作这台小电脑）；
//   别的（控件树、程序内部状态、模拟器状态）不在这里 —— 那属于"开卷"，见玩法任务书 `prompt.md` 的红线。
declare const __jz: {
    computeruse: {
        /** 截图：**整块虚拟桌面**的 PNG data URL（"看"的唯一入口） */
        screenshot(): string;
        /**
         * 归一化坐标（0~1，相对整块桌面）单击；命中窗口会把它**激活**。
         * ⚠ 参数**不能省**（绑定是全参）：少了 btn 会抛 "参数错误:需要3个参数"。
         */
        click(x: number, y: number, btn: 'left' | 'right' | 'middle'): boolean;
        doubleClick(x: number, y: number, btn: 'left' | 'right' | 'middle'): boolean;
        drag(x1: number, y1: number, x2: number, y2: number): boolean;
        /**
         * 按一下键（**Win32 键名**：'Z' / 'Return' / 'Shift' / 'Right'，组合写 'Ctrl+C'）。
         * ⚠ 语义是"按下 + 立刻松开"的一瞬间（与真人点一下键盘同义）；要"按住"得按帧重复调用。
         */
        key(name: string): void;
        type(text: string): void;
        /** 阻塞等（毫秒）—— 在脚本自己的线程上睡，游戏机在另一个线程照跑 */
        wait(ms: number): void;
        /** 像素/文字兜底查找（模板匹配要 OpenCV、OCR 要模型；没接后端时返回 null，不假装成功） */
        match(locatorJson: string): { x: number; y: number; w: number; h: number } | null;
        /** 目标窗口的可访问布局（信封 {source,tree} 或树；找不到窗口 → null）。可当"窗口在不在"用 */
        layoutTree(optsJson: string): unknown;
        waitStable(optsJson: string): boolean;
        launch(optsJson: string): boolean;
        ocrReady(): boolean;
        ocrTexts(): unknown;
    };
};
