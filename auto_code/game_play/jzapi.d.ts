// play.ts 用到的**引擎注入面**的类型声明（**模板**：驱动每次把它拷进工作区的 `play/`）。
//
// 单一来源（改这两处要一起改）：
//   JZJSEngine/JZJSComputerUseModule.cpp 的 registFunction 列表
//   JZComputerUse/JZComputerUse.h 的语义（**物理像素**坐标 / 键名是 Win32 键名）
//
// ⚠ 这份声明刻意**窄**：脚本能用的就是 computer use 这一套（看画面 + 操作这台小电脑）；
//   别的（控件树、程序内部状态、模拟器状态）不在这里 —— 那属于"开卷"，见玩法任务书 `prompt.md` 的红线。
declare const __jz: {
    computeruse: {
        /** 截图：**整块虚拟桌面**的 PNG data URL（"看"的唯一入口） */
        screenshot(): string;
        /**
         * **物理像素**坐标（相对整块桌面，= 截图那张图的像素空间）单击；命中窗口会把它**激活**。
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
        /**
         * **颜色查找·一次多色 + 一次多块**（一帧要同时看几样东西时用它，省往返——贵的是取帧）。
         * opts: `{colors?: string[] | color?: string, region?: {x,y,w,h},
         *         all?: boolean, minW?: number, minH?: number, limit?: number, tol?: number}`
         *   · `colors` 多个颜色**一遍扫描同时判**（成本与单色几乎一样）；
         *   · `all: true` 返回**所有**连通块（面积降序），否则只回最大的那一块；
         *   · `minW/minH` 尺寸下限（小碎块当噪声丢掉，不必再让脚本自己筛）；`limit` 限量；
         *   · 坐标/尺寸一律**物理像素**（与 screenshot/click 同口径）。
         * 返回：`all` 时 `[{x,y,w,h,area,color}, …]`（无命中 = 空数组）；否则单个对象或 `null`。
         */
        matchAll(optsJson: string): unknown;
        /**
         * 屏有多大 / 缩放多少（**物理像素** = logical × scale）。脚本要"点在正中"这类算术时用它
         * （如 `physical[0]/2`）；坐标本身就已是物理像素，**不需要换算**。
         */
        screenInfo(): {
            virtual: boolean; logical: number[]; physical: number[];
            scale: number; dpi: number; explicit: boolean;
        };
        /** 目标窗口的可访问布局（信封 {source,tree} 或树；找不到窗口 → null）。可当"窗口在不在"用 */
        layoutTree(optsJson: string): unknown;
        /**
         * **把窗口带到前台**（最小化先还原）。返回"它现在真是活动窗口吗"（不是"找到了"）。
         * ⚠ 窗口在前台是"注入点击落在它身上"的前提；不在前台时点击会落到上层窗口上。
         */
        focusWindow(title: string): boolean;
        /**
         * **启动一个程序**。真桌面 = 起一个外部进程（path 是 exe）；虚拟桌面（容器）里 =
         * 在桌面上**加载一个 JS 程序**（path = JS 文件路径，程序名 = 文件名去扩展名）。
         * 返回"起来了没有"（⚠ 虚拟桌面下没有系统 pid，所以只回 true/false，别当 pid 用）。
         * 想"起来 + 等它的窗口出现并置前"就直接用 `launch({file, title})`。
         */
        startProcess(path: string): boolean;
        /** 按**进程名**把它的窗口带到前台（容器里就是那个程序的名字，如 "shooter"） */
        focusProcess(name: string): boolean;
        /** 按**进程名**停掉那个程序（它的窗口一起收走）。虚拟桌面下 = 停掉那个 JS 进程 */
        terminateProcess(name: string): boolean;
        /**
         * **窗口表**（z 序：从下到上，末尾最上）。坐标一律**物理像素**（与 screenshot/click 同口径）。
         * opts: `{title?: string}` —— 按标题片段过滤（不给 = 全部）。
         * 返回 `[{handle,index,title,frame,client,visible,active,minimized,maximized}]`；
         * `handle` 是不透明的稳定标识（原样带给 windowState；index 会随建窗/关窗变）。
         * ⚠ 参数**不能省**（绑定是全参，同 click）：不筛就传 `'{}'`。
         */
        windows(optsJson: string): unknown[];
        /**
         * **改窗口状态 / 几何**（SetWindowPos / ShowWindow / SetWindowText 那一组）。
         * opts: `{key, action?, x?, y?, w?, h?, title?}`
         *   · `key`    = `handle`（原样带回）| 标题片段 | 下标（"#2" / "2"）
         *   · `action` = `activate` / `minimize` / `maximize` / `restore` / `close`（缺省 = 只改几何/标题）
         *   · `x,y`    = **外框**位置（物理像素）;  `w,h` = **客户区**尺寸（物理像素）
         *   · `title`  = 改标题
         * 返回改完的那个窗口对象（同 windows() 的一项）；找不到窗口 / 参数不对 → null。
         * ⚠ `close` 是**投递**语义（同点标题栏的 X）: 响应时它可能还在表里，要确认就再查一次。
         */
        windowState(optsJson: string): unknown;
        waitStable(optsJson: string): boolean;
        /**
         * **启动 / 激活目标窗口**：先启动，再等窗口出现并置前（等不到 → false）。
         * opts: `{file?|exe?, args?, title?, timeoutMs?}`
         *   · 真桌面：`exe` = 外部程序路径；
         *   · 虚拟桌面（容器）：`file` = 桌面上要**加载的 JS 文件**（程序名 = 文件名去扩展名）；
         *   · `title` 空 = 只启动、不等窗口。
         */
        launch(optsJson: string): boolean;
        ocrReady(): boolean;
        ocrTexts(): unknown;
    };
};
