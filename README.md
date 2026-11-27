# JZFramework

C++14 + CMake 的跨端基础框架。一套代码里同时有：

- **基础设施**：对象模型（属性/信号槽/线程归属）、容器与字符串、定时器、进程/文件/日志、网络、JSON/XML、压缩、数据库；
- **桌面自绘 UI**（Windows）：控件树 + 绘制任务流，渲染后端可换（GDI+ / Direct2D），带样式表、布局、动画、IME、AI 语义树；
- **2D 游戏层**：`JZWidget` 之上的固定步长循环、精灵/场景、AABB 碰撞、滚屏相机，单独编成 `JZGame.dll`；
- **JS/TS 运行时**：QuickJS-ng + 一套 TS 包部署规范，C++ 与 JS 双向绑定；
- **LLM Agent 运行时**：`JZAgent` 核心 + `agent_cli` / `agent_node` / `agent_cli_server`，带 HTTP/SSE 契约、任务库、工具与技能。

## 快速开始

前置：**VS2022（MSVC x64）+ CMake ≥ 3.10 + Python3**（构建期有代码生成步骤）。
UI 与游戏部分（`JZWidget` / `JZGame` / `JZEmulator`）只有 Windows 实现；其余模块在代码里保留了
Linux/macOS 与 ESP32(FreeRTOS) 分支（`JZ_OS_LINUX` / `JZ_OS_FREE_RTOS`），但本仓库的构建脚本与 CI
目前都只按 Windows 维护。

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

常用变体：

```bash
cmake --build build --config Debug --target test_game_scroll   # 只编一个目标
cmake -S . -B build -DJZFRAMEWORK_DYNAMIC=OFF                  # 编静态库（默认是动态库）
cmake -S . -B build-asan -DJZ_ENABLE_ASAN=ON                   # AddressSanitizer（建议单独目录）
```

## 目录与产物

| 目录 | 内容 |
| --- | --- |
| `JZCore/` `JZNet/` `JZJson/` … | 基础模块（下表），编进 `JZFramework.dll` |
| `JZWidget/` | 自绘 UI（控件/布局/绘制/窗口后端） |
| `JZGame/` | 2D 游戏层，**单独 DLL** |
| `JZJSEngine/` | QuickJS-ng 绑定 + TS 运行时 |
| `JZAgent/` `JZMcpServer/` | Agent 核心与 MCP 服务端 |
| `binary/` | 可执行程序：`agent_cli`、`js_runtime`、`frpc`、`updater`、`virtual_device`、`jzpdf` |
| `test/` | 全部测试与示例（`test/widget`、`test/game`、`test/jsengine`、`test/agent` …） |
| `tools/` | 构建期工具（`jz_uic` 等） |
| `docs/` | 设计文档（见文末） |

exe 只有两个落点：

- 发布程序：`build/binary/bin/<配置>`
- 测试/工具：`build/test/bin/<配置>`

## 模块一览

**基础**

| 模块 | 说明 |
| --- | --- |
| `JZCore` | 对象模型（`JZObject` 树、属性、信号槽、线程归属）、事件循环、`JZApplication`、定时器、进程/线程/文件/路径、日志 |
| `JZJson` `JZXml` | JSON / XML |
| `JZCompress` | 压缩（zlib 等） |
| `JZDatabase` | SQLite：KV 与 SQL 两套封装 |
| `JZTest` | 测试框架与基准（`JZTestSuite` / `JZBenchmark`） |
| `JZCoroutine` | 协程（自带汇编上下文切换） |

**网络与协议**

| 模块 | 说明 |
| --- | --- |
| `JZNet` | TCP/UDP、HTTP 客户端、WebSocket、MQTT、TLS（OpenSSL） |
| `JZHttpServer` | HTTP/SSE 服务端 |
| `JZMcpServer` | MCP（Model Context Protocol）服务端 |
| `JZFrp` | 内网穿透（`jzfrpc` / `jzfrps`） |
| `JZModbus` | Modbus 主/从 |
| `JZOTA` | 固件/文件升级 |

**设备与多媒体**

| 模块 | 说明 |
| --- | --- |
| `JZAudio` | 音频采集/播放 |
| `JZCamera` | 摄像头取帧 |
| `JZSerialPort` `JZGpio` `JZSensor` | 串口、GPIO、传感器（均带 ESP32 驱动实现） |
| `JZCv` | 视觉/OCR（OpenCV + ONNX Runtime，PaddleOCR；单例模块，C++17） |
| `JZComputerUse` | 屏幕/鼠标/键盘操作后端（含虚拟桌面） |
| `JZEmulator` | GBA 画面控件（mGBA 核心，外部 `mgba_host.dll`） |

**UI 与游戏**

| 模块 | 说明 |
| --- | --- |
| `JZWidget` | 控件树、布局、样式表、动画、IME、绘制任务流；渲染后端 `JZGdiBackend`（默认）/`JZD2DBackend` |
| `JZGame` | `JZGameView`（固定步长+渲染双定时器）、`JZGameView2D` + `JZCamera2D`（滚屏）、`JZSprite`/`JZScene`/`JZBody`；**独立 `JZGame.dll`** |

**JS 与 Agent**

| 模块 | 说明 |
| --- | --- |
| `JZJSEngine` | QuickJS-ng 绑定、TS 包运行时、C++↔JS 绑定层 |
| `JZAgent` | LLM Agent 核心：上下文历史、工具调用、验证循环、压缩、多模态适配层 |
| `JZNode` | 节点/数据流模型（`JZNodeFlow`） |

## 作为库使用

```bash
cmake --install build --config Release --prefix build/install
```

外部工程：

```cmake
find_package(JZFramework CONFIG REQUIRED)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE JZFramework::JZFramework)

# 需要 2D 游戏层时（单独 DLL，导入目标会带出头文件与 JZGAME_DYNAMIC）
target_link_libraries(app PRIVATE JZFramework::JZGame)
```

`JZFRAMEWORK_DYNAMIC` / `JSON_DLL` / `HAS_JZ*` 等宏由导入目标自动带出。
运行时把 `bin/` 下的 `JZFramework[d].dll`（以及 `JZGame[d].dll`、OpenSSL 依赖）放到 exe 旁边即可。

示例工程可参考 `build/agent_env/workspace/cli3/{snake,tetris}`（消费安装包）。

## 跑几个东西看看

```bash
# 横版滚屏示例：自动跑一遍，打印相机位置/可见数，并抓一张 PNG
build/test/bin/Debug/test_game_scroll.exe --auto --shot scroll.png

# 自绘 UI 控件巡览
build/test/bin/Debug/test_widget.exe

# Agent 服务端（网页管理台在 http 端口上）
build/binary/bin/Debug/agent_cli_server.exe -c build/agent_env/configs/server.json

# 测试
ctest --test-dir build -C Debug
```

环境变量：`JZ_RENDER_BACKEND=d2d|gdi` 切渲染后端（默认 gdi）；`JZ_LOG_LEVEL=debug|info|warn|error|fatal` 控制日志等级。

## 架构速览

**1. 对象模型**
所有可被子对象化的类型都继承 `JZObject`：父子树负责生命周期，信号槽带"线程归属"（跨线程投递、同线程直调），`JZTimer`/`JZFixedTimer` 提供事件循环定时（固定步长带补帧）。窗口/控件/定时器都在同一棵树里，所以析构顺序是确定的。

**2. 渲染**
控件 `paintEvent` 只**产生绘制任务**（`JZDrawTask`：Rect/Border/Line/Arc/Path/Text/Image/Pixmap + Push/Change/Pop 状态机），`JZLayer` 收集后交给 `JZRenderBackend` 回放：

```
paintEvent ──► JZPainter ──► JZLayer(tasks) ──► JZRenderBackend
                                               ├─ JZGdiBackend   (DIB + GDI+, 默认)
                                               └─ JZD2DBackend   (ID2D1HwndRenderTarget + DirectWrite)
```

两套后端语义对齐（同一份任务列表），像素布局同为 32bpp PARGB，贴图零转换；离屏/虚拟桌面走 GDI 的 `targetPixels` 零拷贝取帧。

**3. JS/TS**
QuickJS-ng（仅 ESM），TS 包按固定 5 步部署（包目录、`builtinModules[]`、tsconfig references、types、运行时说明）；C++→JS 唯一入口 `callJs()`。

**4. Agent**
`JZAgent`（一次任务内多轮 LLM + 工具 + 验证循环）之上是三种可执行体：`agent_cli`（单机，自带 HTTP）、`agent_node`（远程节点）、`agent_cli_server`（管理台 + 任务库 + 网页）。契约是 `POST /task`、`GET /events`(SSE)、`POST /reset`，任务状态落 SQLite，会话上下文落 `data/ctx/<id>.json`。

## 平台与可选依赖

| 依赖 | 用途 | 缺省行为 |
| --- | --- | --- |
| VS2022 / MSVC x64 | Windows 构建 | — |
| Python3 | 构建期代码生成 | 缺失直接报错 |
| OpenSSL | TLS（`MG_TLS_OPENSSL`） | 框架已内置链接 |
| OpenCV + ONNX Runtime | `JZCv`（OCR） | 找不到就不建该模块 |
| PDFium（`-DJZ_PDFIUM_DIR=`） | `jzpdf` | 留空则只有 docx/xlsx 可用 |
| `mgba_host.dll`（`-DJZ_MGBA_DIR=`） | `JZEmulator` | 找不到就不建该模块 |

## 约定（读代码前值得知道）

- 源文件 **UTF-8**；注释用中文，头文件带分节说明。
- 头文件按模块放，`#include "JZxxx.h"` 是裸文件名（安装包把所有模块目录都加进搜索路径）。
- 动态库默认开（`JZFRAMEWORK_DYNAMIC=ON`），导出只认显式标记（`JZFRAMEWORK_API` / `JZGAME_API`）；消费方必须用**同一工具集、同一 CRT、同一配置**重编。
- 日志等级用环境变量 `JZ_LOG_LEVEL`，在 `JZLogManager::log()` 入口早退；`LOG_FATAL` 不受等级控制。
- 改了框架源码要重新编译（含 DLL 拷贝到 exe 目录，构建脚本已处理）。

## 设计文档

`docs/` 下按主题记录：`agent-hub-design.md`（agent 拓扑与协议）、`web-ui-design.md`、`ai-native-design.md`（UI 语义树/AI 驱动）、`jzstylesheet-design.md`、`compute-use-design.md`、`code-agent-design.md`、`js-engine-wrapper-design.md`、`jzshell-lib-api-design.md`、`web-search-design.md`、`mobile-ui-design.md` 等。

## 许可证

仓库内暂无 LICENSE 文件；如需对外发布请先补充。
