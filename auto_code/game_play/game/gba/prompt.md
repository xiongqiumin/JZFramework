# gba 玩法 —— 备用玩法（**没有自动判据**，代码保留）

容器里两个进程：**`gba`**（题目侧的模拟器，不许改）+ **`script`**（`play/play.ts`）。

- 窗口 `GBA`，**240x160**（1:1 上屏，不放大不超分）；桌面 320x240；要 ROM。
- 键位要**翻译**成键盘名（权威映射是 `JZEmulatorView::gbaKeyOf`，脚本里是 `GBA_KEY` 那张表）：
  机身上的 `A`/`B`/`Start`/`Select`/`L`/`R` → 键盘 `Z`/`X`/`Return`/`Shift`/`A`/`S`；方向键同名。
  ⚠ 用**机身键名**翻译，别直接猜键盘字母（`A`/`S` 这两个字母在容器里是 L/R）。
- ⚠ 每局都从**开机 logo** 开始：`JZEmulator` 只暴露 `openRom/start/stop/frameCount`，没有读/写存档的接口。

**判分**：没有自动判据（整部通关太难）—— 尺子没有，退出码不表示"通关"；要评只能人看画面。
API（`__jz.computeruse.*`）、红线、产出契约（`play/play.ts` + `<out>/session.json` + `<out>/frames/`）、
参数（运行根 `play.json`）与 hello 玩法一致，见 `game/hello/prompt.md` 的对应小节。
