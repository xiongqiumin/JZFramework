// ==================== 游戏机 gba.ts（GBA 模拟器玩法；题目侧，不是被测实现） ====================
// 容器里跑的一个 JS 程序：开一个 240x160 的窗口 → 里面放 JZEmulatorView → 打开 ROM → 开跑。
// 它是对面的"被测 App"：脚本（play/play.ts）只能像人一样**看这块屏、按这个键盘**，
// 看不到这里面的任何状态（ROM 路径、帧数、内存……都在容器里，脚本够不着）。
//
// 为什么要有它（而不是给 virtual_device 加 --mgba <rom>）：这是**任务侧**的东西 ——
// 它决定"这一局是什么机器/什么分辨率/怎么喂键"，换个玩法（联机、存档、录像）只改这里，
// 容器那一层仍是通用容器（见 binary/virtual_device/JZVirtualDeviceHttp.h）。
//
// 参数从运行根（= 本 js 所在目录 build/game）的 game.json 读，由 env.py 写：
//   { dllDir, rom, intervalMs, statusPath }
//
// 状态文件（statusPath）写给**驱动**看：ROM 开没开 / 跑了几帧 / 报的什么错。
// 它是"链路故障"与"游戏玩得不好"的分界（后者不归尺子管，这一版不自动判分）。
import fs from 'fs';

interface GameCfg {
    dllDir: string;      // mgba_host.dll 所在目录（它的三个 MinGW 运行库要在同一目录）
    rom: string;         // ROM 路径（.gba）
    statusPath: string;  // 状态文件（绝对路径）
}

function main(): void {
    const runDir = jzApp.applicationDirPath();
    const cfg = JSON.parse(fs.readFileSync(runDir + '/game.json', 'utf8')) as GameCfg;

    // 窗口客户区就是 GBA 原生画面 240x160（1:1 上屏，不放大不超分 —— 见 JZEmulatorView.h）
    const win = new JZWindow(240, 160, 'GBA');
    const view = new JZEmulatorView(win);
    view.setGeometry(0, 0, 240, 160);
    win.show();       // ⚠ 先 show 窗口再 show 控件：showEvent 里控件会 setFocus（抢键盘）
    view.show();

    const opend = view.openRom(cfg.dllDir, cfg.rom);
    const error = opend ? '' : view.lastError();
    if (opend)
        view.start();     // ⚠ JS 绑定是 0 参数（节拍固定 16ms ≈ 60Hz，见 JZEmulatorViewJs.cpp）

    const status = () => ({
        rom: view.isRomOpen(),
        openRom: opend,
        error: error || view.lastError(),
        running: view.isRunning(),
        frames: view.frameCount(),
        w: 240, h: 160,
        ts: Date.now(),
    });
    const write = () => {
        try {
            fs.writeFileSync(cfg.statusPath, JSON.stringify(status()) + '\n', 'utf8');
        } catch (e) {
            // 状态文件写不出去不该影响玩：静默（驱动那边会看到"没有新状态"）
        }
    };

    // 顶层窗口必须自己保活（没人引用 ⇒ 被 GC ⇒ 窗口销毁 ⇒ 程序退出）
    (globalThis as any).__gba = { win, view, status };

    write();
    setInterval(write, 200);

    console.log('[gba] openRom=' + opend + (error ? (' err=' + error) : '')
        + ' rom=' + cfg.rom + ' dllDir=' + cfg.dllDir);
}

main();
