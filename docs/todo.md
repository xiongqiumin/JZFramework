# TODO（挂起事项）

> 只记「已经想清楚、但暂时不做」的事。每条写清：为什么、怎么做、怎么验收、动哪些文件。
> 有结论不再改动的「否决项」记在文末，避免下次又被提出来。

---

## T1 渲染后端改成「原生表面」契约（GDI = CPU / D2D = GPU）

**现状**
`JZRenderBackend` 只暴露 `targetPixels()`（CPU 零拷贝视图）。`ID2D1HwndRenderTarget` 给不出 CPU 指针，
所以离屏设备（`initOffscreen`）拿 D2D 后端时 `init` 直接返回 false，`JZDisplayDevice` 回退 GDI。

**结论**
不该让 GPU 后端为了满足 CPU 契约去绑 DIB（`ID2D1DCRenderTarget`）——那是把 GPU 后端降级成软件光栅。
后端应该交自己的原生表面：GDI 交 DIB，D2D 交 GPU texture。CPU 像素是**某个消费者的需求**，不是后端的义务。

**方案**
```cpp
struct JZRenderSurface {
    enum Kind { None, CpuRgba, D3D11Texture } kind = None;
    void*          texture = nullptr;   // ID3D11Texture2D* / IDXGISurface*
    const uint8_t* pixels  = nullptr;   // 仅 CpuRgba（GDI 的 DIB 就是它）
    int width = 0, height = 0, stride = 0;
    uint32_t format = 0;                // DXGI_FORMAT
};
virtual JZRenderSurface targetSurface() const;   // 新增
virtual const uint8_t* targetPixels(...) const;  // 保留：CpuRgba 的便捷视图
```
消费方按 kind 分派：合成器优先贴同 kind 表面（GPU→GPU 零拷贝），跨 kind 才回读；
截图 / OCR / ComputerUse 走显式 `readback()`（按需、可异步）。

**分三步**

| 步 | 内容 | 风险 |
|---|---|---|
| ① | 接口扩成 `targetSurface`；GDI 返回 `CpuRgba`、D2D-window 返回 `D3D11Texture`；合成器遇到 GPU 表面先走回读兼容（行为=今天） | 低，纯接口 + 兼容分支 |
| ② | D2D 加 GPU 离屏（`ID2D1DeviceContext` + `D2D1Bitmap1`），虚拟窗口用它；虚拟桌面合成器加 GPU 分支 | 中：合成器要从 CPU 合成改成 D3D 合成 |
| ③ | 截图 / OCR 改成显式 `readback()`（staging texture + 按需 map） | 低，但要把「帧随时可读」的假设全找出来改掉 |

**约束（写进实现注释）**
- 虚拟窗口在自己的线程渲染 ⇒ GPU 表面跨线程写入需要 keyed mutex / fence；CPU 缓冲天然安全。
- `JZImage` / `JZPixmap` 在 UI、OCR、emoji、JS canvas 里到处都是 CPU 类型，不能让这一层变 GPU；
  边界是「表面层用原生表面，图像层保持 CPU，桥接只有 readback」。

**验收**
`virtual_device` 用例在 `JZ_RENDER_BACKEND=d2d` 下也通过；`targetSurface()` 在虚拟窗口上返回 GPU 表面，
且截图仍能拿到像素。

**动哪里**：`JZWidget/JZRenderBackend.h`、`JZWidget/JZDisplayDevice.{h,cpp}`、`JZWidget/JZD2DBackend.{h,cpp}`、
`binary/virtual_device/*`。

---

## T2 全量测试复核（D2D 已改为默认）

**现状**
翻默认值后跑过一次全量：59 项里 58 通过，唯一失败是 `virtual_device`（`host.device()->snapshot()` 在 D2D 下恒 false）。
已补 `JZD2DBackend::snapshot()`（PrintWindow + `PW_RENDERFULLCONTENT`）并单测通过；**其余 58 项没有重跑**。

**待办**
1. `ctest --test-dir build -C Debug` 全量一次；
2. `JZ_RENDER_BACKEND=gdi` 下再全量一次（确认老后端没被改坏）；
3. 最好在**非 D2D 机器**（老 Windows / 无 GPU）上冒烟，验证自动回退 GDI。

**验收**：两种后端各 59/59。

---

## T3 D2D 视觉基线

**现状**：文本（DirectWrite vs GDI+）、圆角与描边的亚像素细节不完全一致；现有 UI 基线都是 GDI+ 口径。

**待办**
- 手工巡一遍 `test_widget.exe` 的主要控件（文本、按钮、表格、滚动区、输入框、菜单）；
- 若有像素级截图基线，补一份 d2d 的，或引入容差；
- 需要逐像素对齐的场合，用 `JZ_RENDER_BACKEND=gdi` 兜底。

---

## T4 JZGame 安装包验证（Release + `build/install`）

**现状**：`JZGame` 已改成独立 DLL 并进默认构建，`JZFramework::JZGame` 导入目标写好了，
但**还没有真实外部工程消费过**；上次 Release/install 验证被打断。

**待办**
1. `cmake --install build --config Release --prefix build/install`；
2. 检查 `bin/JZGame.dll`、`lib/JZGame.lib`、`include/JZFramework/JZGame/**`；
3. 拿 `build/agent_env/workspace/cli3/snake` 那类 `find_package` 工程链 `JZFramework::JZGame` 编一次，
   运行确认 DLL 在 exe 旁、`JZGAME_DYNAMIC`/`HAS_JZGame` 正确带出。

**验收**：外部工程零改动即可链接；Debug/Release 后缀（`JZGame.dll` / `JZGamed.dll`）都对。

---

## T5 `sprite_bench --windowed` 的用法边界

**现状**：新增窗口模式（测真后端）。离屏模式只能测 GDI-DIB，测不到 D2D。

**注意**
- D2D 在 1000 精灵档被 vsync 锁住（p50 恰好 16.67ms），要测真实上限得用更大的 N 或关 vsync；
- 若以后要长期跟踪后端性能，把它接进 benchmark 流程（**只报数、不设门限**，机器噪声太大）。

**参考数据**（2026-11-27，Release，DPI 200%）：GDI 68~115 μs/精灵，D2D 7.3~15.5 μs/精灵。

---

## T6（远期）虚拟桌面 GPU 合成

依赖 T1 的 ②③。当前虚拟桌面是 CPU 合成（z 序 / 缩放 / OCR 标注都在 CPU 侧）。
动手前先接受：目标帧不再「随时可读」，截图/OCR 全部改成按需异步回读。
只有当「虚拟桌面里跑高频重绘」成为真实需求时才值得做。

---

## 否决项（已有结论，别重复提）

| 提案 | 否决理由 |
|---|---|
| 给 D2D 后端实现 `ID2D1DCRenderTarget` + DIB，以支持离屏 | 那是让 GPU 后端走 WARP 软件光栅，方向错了；正确做法是 T1 的原生表面 |
| 窗口级控件引用（焦点/鼠标抓取/悬停/输入屏蔽）改成 `JZObjectPointer` 弱引用 | 弱引用失效点仍在 `~JZObject`，等于还靠析构钩子，却要多付出处处判空、类型转换与热路径开销；`JZObjectPointerGuard` 还会拿 `globalLock`（有自锁死风险）。裸指针 + 两个入口（`~JZWidget` / `setParentWidget`）清理是穷尽的 |
| 给 `m_focusWidget` 等改成「焦点路径索引」 | 所有派发路径要重算、树变了要修，比现状更容易错 |
| JZWidget 全面 Pimpl（消 C4251） | 交付前提改成「同工具集 + 同 CRT + 同配置重编」，不为此重构几百个导出类 |
