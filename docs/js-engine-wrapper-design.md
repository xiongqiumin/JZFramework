# JZ JS 引擎封装设计 (JZJSEngine / js_wrapper)

> 2026-09-22 首次成文：把散在 `JZJSEngine.cpp` 表头注释、`js_wrapper/types/index.d.ts`、
> 各包 `types/ambient.d.ts`、`JZJSBind.h`、包文件头里的**封装原则**收成一份，
> 作为"加模块 / 加名字 / 改绑定 / 写提示词"时的判据。
> 全部条目都对着**当前代码**写；与实现不一致的地方单独列在 §10（已知不一致）。
>
> 2026-09-22 同日改布局：`net`（8 个名字）、`core`（5 个名字）两个"多名字包"拆成
> **一个名字一个包目录**（目录名 = import 的名字；`os` 不再跨包合并；`bytes` 从"第二入口"独立成包），
> 并给每个名字同时提供默认导出与具名导出（`import { createServer } from 'http'` 因此成立）。
>
> 主题：让模型/用户按 **Node 的写法**写代码（裸名 `import`、同名 API、Promise、EventEmitter），
> 而底层是 C++ 框架（JZObject / 控件 / 网络 / SQLite）—— 封装层只做"翻译"，不发明新 API。

---

## 1. 目标与边界

**做**：

- 用户/模型只写 Node 风格代码：`import http from 'http'`、`fs.readFileSync`、`new DatabaseSync(...)`；
- 类型能被 tsc 正确检查（对齐 `@types/node` 的组织方式：**按名字给类型**）；
- 运行时零构建：源码 `.ts` 由 `tsc -b js_wrapper` 出 `dist/`，引擎按名字/路径解析加载。

**明确不做**（避免"半个 npm"）：

- ⛔ `node_modules` / 包管理 / npm 包加载 / `package.json` 的 `exports` 解析；
- ⛔ CommonJS：没有 `require`，不认 `module.exports` / `exports`（写了就是 ReferenceError）；
- ⛔ DOM / 浏览器 API（除少数 WHATWG 全局，见 §6）；
- ⛔ 在 JS 层重写算法：C++ 侧是唯一实现，JS 层只做形状转换（见 §5）。

**边界划分**（三层，各管一段）：

| 层 | 位置 | 职责 |
| --- | --- | --- |
| 引擎 | `JZJSEngine/` 的 C++ | 模块解析、类型表、绑定（`jsbind`）、全局装配 |
| 原子 API | C++ 注册的 `__jz.*` 命名空间 | 只提供"能力"（能连、能读、能执行 SQL），不管 Node 口味 |
| 运行时包 | `js_wrapper/<名字>/index.ts` → `dist/` | Node 语义的抹平：命名、参数形状、错误对象、返回形态 |

---

## 2. 名字 = 契约（核心原则）

**裸名（bare specifier）就是 Node 的内置模块名**，由 `JZJSEngine.cpp` 的 `builtinModules[]`
**白名单**决定（不是按路径找文件，所以比 Node 的解析更快）。
`normalizeBuiltinName()` 会剥掉 `node:` 前缀 ⇒ `node:http` 与 `http` 同表命中。

```cpp
// JZJSEngine.cpp —— 内置名表（16 个名字，2026-09-22 一名字一包之后）
struct BuiltinModule { const char* name; const char* file; const char* exportName; };

{ "net",   "net/dist/index.js",   nullptr },   // nullptr = 用包自带 default
{ "dgram", "dgram/dist/index.js", nullptr },   //   包 = 目录名 = 名字
{ "http",  "http/dist/index.js",  nullptr },
{ "os",    "os/dist/index.js",    nullptr },   // 以前跨包合并，现在一个包
{ "buffer","buffer/dist/index.js",nullptr },
{ "sqlite","sqlite/dist/index.js",nullptr },
{ "widget","widget/dist/index.js","*"     },   // "*" = namespace 形态（包入口没有 default）
```

⭐ **一个名字 = 一个包目录**（`js_wrapper/<名字>/{index.ts, types/, dist/}`）：
表里 `file` 就是"它自己的 dist 入口"，不再有"名字 A 藏在包 B 里"（以前 `http` 在 `net` 包里、
`os` 还要跨两个包合并）。查定义时"名字 → 目录 → 类型文件"三者同名。

### 2.1 三处同步（加名字时必须一起改）

1. **本表**（`JZJSEngine.cpp` 的 `builtinModules[]`）；
2. **该包的 `types/ambient.d.ts`**：`declare module '<名字>' { export * from './index' ... }`；
3. **类型清单与提示词**：`js_wrapper/types/index.d.ts` 的内置名列表 + `jzTsRuntimeNotes()`
   （`binary/agent_tool/JZCodeTools.h`，实现在 `.cpp`）；
4. （工程侧）`js_wrapper/tsconfig.json` 的 `references` 加新包目录。

> ⚠ 缺第 2 处的症状是**编译期 TS2307（名字没有类型声明），运行期却是好的** ——
> 模型会被白绊一次，还找不到原因。这就是"名字 = 契约"的代价：契约有三份，缺一不可。

### 2.2 两种导出形态（按名字分，不由使用者选）

| 形态 | 判据（表里的 `exportName`） | 用法 |
| --- | --- | --- |
| 有 default | 具名导出名 / `nullptr`（用包自带 default） | `import http from 'http'`；`import SerialPort from 'serialport'` |
| namespace | `"*"` | 具名导入：`import { DatabaseSync } from 'sqlite'`、`import { Worker } from 'worker_threads'` |

桥接（`builtinBridgeCode()`）在 loader 层**合成一段 ESM 桥接源码**，把"包出口里的某个导出"
提升成 `default`（`"*"` 时生成 `export * from ...`），所以不会出现"类型写着可用、运行时不认"的漂移。

### 2.3 `exportName`：包有没有 default

- `nullptr`：包自带 `export default <模块对象>`（现在的常规形态，16 个名字里 15 个如此）⇒
  `import http from 'http'` 与 `import { createServer } from 'http'` **都成立**
  （桥接同时给 `export *` 与 `export { default }`；这修掉了旧版"具名导入子成员编译过、运行崩"的坑）；
- `"*"`：namespace 形态（包入口只有具名导出）—— 目前只有 `widget`（控件类层是 `export *` 一堆类）。

---

## 3. 只支持 ESM

- 引擎用 `JS_SetModuleLoaderFunc`（`JZJSEngine.cpp:537`）挂 `JSInter::js_module_loader`；
- 前缀 `/` 或 `.` ⇒ 当文件模块；否则查内置名表；都命不中 = 模块找不到；
- 文件模块的候选：`<name>`、`<name>.js`、`<name>/index.js`（`readModuleCode()`，
  **先 workDir 再 `libraryDirs`**）；`libraryDirs` 由宿主/`initProject` 指到产物目录。

⚠ **解析基准是产物目录（`--root`），不是源码目录** —— 所以包里互相 import 一律用
"相对 jz_js 根的裸路径"（`import { os as netOs } from 'net/dist';`），部署时目录同构就原样可解析。

---

## 4. 包结构：类型与实现同源

```
JZJSEngine/js_wrapper/
  types/                    # 共享声明（不属于任何名字）
    index.d.ts              #   入口 + 契约说明（本文件是"文档的活版本"）
    globals.d.ts            #   全局类 URL / URLSearchParams / WebSocket / self()
    fetch.d.ts              #   WHATWG 全局 Headers / Response / RequestInit / fetch
    console.d.ts / timers.d.ts / events.d.ts / common.d.ts
  internal/                 # **不是**内置名：各包实现互相 import 的工具（toBytes/fromBytes）
  <名字>/                   # 一个名字 = 一个包目录（net/ http/ fs/ sqlite/ ...）
    index.ts                #   运行期实现（tsc 编出 dist/index.js）
    types/index.d.ts        #   模块类型 —— **单一来源**，与实现同目录
    types/ambient.d.ts      #   只声明裸名并转发（declare module '<名字>'）
    package.json / tsconfig.json
  widget/                   # 控件族（特殊：包入口由引擎按路径加载，另注册为 namespace 名字）
  package.py                # 打包/部署（见 §7.3）
```

**为什么 ambient 要单独一个文件**：`declare module '<裸名>'` 只能写在**全局脚本文件**里
（无顶层 `import`/`export`）。`fs.d.ts` 是模块文件，在模块里写 `declare module 'fs'` 会被当成
"模块增强"，要求目标模块已存在 ⇒ 不成立。所以：

```ts
// <pkg>/types/ambient.d.ts —— 只做"名字 -> 模块类型"的转发
declare module 'fs'    { export * from './fs'; }
declare module 'url' { export * from './url'; }   // 第二入口同样要有名字
```

**单一来源的红线**：类型**不允许**和实现分家（历史上 `JZJSEngine/types` 与包内 `dist` 漂移过，
现在只有 `js_wrapper` 一处）。改实现就改同目录类型，改完跑 `tsc -b js_wrapper`。

---

## 5. 分层：C++ 原子 API → JS 薄封装

C++ 只往 `globalThis.__jz` 上挂"原子能力"，形状**不迁就 Node**：

```ts
declare const __jz: any;
// __jz.core.* / __jz.net.* / __jz.http.createServer / __jz.ws.connect
// __jz.bytes.* / __jz.sqlite.* / __jz.serialport.* / __jz.modbus.* / __jz.worker.* / __jz.os.networkInterfaces
```

Node 语义由**包内 `index.ts`** 抹平，原则是"**只翻译，不实现**"：

- **命名与参数形状**：位置参数 / 尾部对象选项 / 默认值（Node 怎么写就怎么写）；
- **错误对象**：底层抛的是原生异常字符串 ⇒ JS 侧补成带 `code` / `syscall` / `path` 的
  Node 风格 `Error`，让调用方能写 `catch (e) { e.code === 'ENOENT' }`；
- **返回形态**：`Stats` 对象（`isFile()` / `mtime` ...）、迭代器（`iterate()`）、
  `{ changes, lastInsertRowid }` 这类结构在 JS 侧组装；
- **合并**：`os` 一半在 core（EOL 等）、一半在 net（`networkInterfaces`），按 Node 语义合成一个对象；
- **跨包复用唯一实现**：如 `EventEmitter` 只在 `net` 包实现一次，其它包
  `import { EventEmitter } from 'net/dist'`，不许各抄一份。

**每个包文件头必须写清"与上游的一致 / 差异 / 未实现"**（例：`sqlite/index.ts` 声明了
全同步 API 对齐 `node:sqlite`、参数 `?`/具名、以及 function/loadExtension/changeset **未实现**）。
这是封装层最重要的文档：使用者（尤其模型）靠它判断"能不能这么写"。

---

## 6. 全局面：谁该是全局，谁必须是模块

**判据 = 在 Node 里它是不是全局**。是全局的才留全局，否则一律走模块（只 `export`，必须 `import`）。

**引擎注册的全局**（`registConsole()` / `initJSEnv()`）：

| 全局 | 说明 |
| --- | --- |
| `console` | log / error / warn / debugPrint / assert（同一 sink，走 `sigLog`）。log/error/warn/assert 的输出格式 = **node 的 `util.format` + `util.inspect`**（顶层字符串不加引号、`{ a: 1 }` / `[ 1, 2 ]`、深度 2、`%s/%d/%j/...` 占位符）；`debugPrint` 是 JZ 原来的紧凑格式（字符串带引号、`{a: 1}` 无内边距、能列出继承来的可枚举属性） |
| `sleep` / `setInterval` / `setTimeout` / `clearInterval` / `clearTimeout` | 引擎定时器（离引擎表就不转） |
| `self<T>()` | 取"当前正在被 JS 调用的那个对象"的包装（箭头函数回调里没有 `this` 时用它） |
| `engine` | `engine.isInterruption()`（长循环里配合取消） |
| ⚠ `process` / `fs` | **遗留全局**：只有 `process.cwd()` 与 `fs.readString()` 两个成员，属历史包袱，不要依赖 |

**由各包实现并挂到 `globalThis` 的 Web 平台全局**（"谁实现谁挂"）：

| 全局 | 实现所在包 |
| --- | --- |
| `EventEmitter`（Node 的，为兼容也挂全局） | `events/` |
| `URL` / `URLSearchParams` | `url/` |
| `WebSocket` | `ws/` |
| `fetch` / `Headers` / `Response` | `http/` |

挂载方式：`JZJSNetModule::initEngine()` 在**注册时**按契约相对名依次副作用装载
`events/dist/index.js` → `url/dist/index.js` → `ws/dist/index.js` → `http/dist/index.js`
（各包在自己的模块顶层 `globalThis.X = ...`）。语义与 Node 启动时装 undici 相同：**运行时初始化动作**，
不需要宿主参与，也不取决于磁盘布局。

- ⚠ 模块名必须写"契约相对名"（与 import 链路里的说明符完全一致）：QuickJS 的模块缓存按**模块名**
  索引，用绝对路径会装出**第二份**实例（`globalThis.Headers` 会被后加载的那份覆盖、跨份
  `instanceof` 失败）。用户入口再 import 同名包会命中同一份，不会重复执行。
- ⚠ 走 `JZJSEngine::tryRunModuleFile()`：包没部署/没编译只记一条 `LOG_W`，**不**把引擎打成
  `Engine_error`（否则宿主会当成"JS 崩了"弹窗/中断）。
- ⚠ `worker` 引擎也装 net 模块 ⇒ 同样有这些全局（对齐 Node 的 Worker）。

---

## 7. 类型侧约定

### 7.1 `import type` 才安全

```ts
import type { Socket } from 'net';   // 类型位置 ⇒ tsc 擦除, 不触发模块加载
import fs from 'node:fs';            // 值位置 ⇒ 真正加载模块
```

### 7.2 tsconfig 要求（写进 `initProject` 的模板）

- `"lib": ["ES2022"]`，**不含 DOM** —— 否则 `types/globals.d.ts` / `fetch.d.ts` 里的
  `WebSocket` / `URL` / `Response` / `Headers` 会与 `lib.dom.d.ts` 撞重复标识符；
- `"include"` 要带上 `jz_types/**/*.d.ts`；
- `moduleResolution: bundler` 会让 `allowSyntheticDefaultImports` 默认 true（见 §10）。

### 7.3 部署布局（`package.py`）

```
<exe>/jz_engine/jz_types/types/**        <- js_wrapper/types          （共享声明）
<exe>/jz_engine/jz_types/<pkg>/types/**  <- js_wrapper/<pkg>/types    （模块类型 + ambient）
<exe>/jz_engine/jz_js/<pkg>/dist/**      <- js_wrapper/<pkg>/dist     （运行时 JS）
```

- **与源码目录同构**是刻意设计：包内 `../../types/`、ambient 的 `./fs` 等相对引用
  原样可解析，**不需要**在源码与产物之间改写路径；
- `initProject` 再把 `jz_types/`、`jz_js/` 拷进用户工程（相对引用继续成立）；
- ⚠ TS7(tsgo) 在 `declare module` 内**不支持相对路径的通配再导出**（会静默不导出任何成员）
  ⇒ `package.py` 在部署副本里给每个 ambient 块追加一行**显式具名再导出**
  （源码保持标准写法，TS5/TS7 都可用）；这行是生成物，别去源码里手写。

---

## 8. C++ 绑定侧原则（JS 面是怎么来的）

`jsbind`（`JZJSBind.h` / `JZJSEngine.h`）的约定，改绑定前先读这一节：

1. **禁 C 函数返回裸指针**：`static_assert(!std::is_pointer<ReturnT>::value, "返回值不能是指针, 请在js侧包装")`
   ⇒ 要交对象用 `toJsValueFromPtr(new X())` / `toValuePtr(...)`，让 JS 侧拿到包装。
2. **异常约定**：lambda 里 `JS_ThrowXxx(...)` 后直接 `return;`（挂异常不返回）⇒ `CFunctionImpl`
   检测 `JS_HasException` 并交回 `JS_EXCEPTION`，JS 的 `try/catch` 才捕得到；`create` 工厂统一
   "设 JS 异常 + 返回 nullptr"，不抛 C++ 异常。
3. **探测型 API 不许留下待决异常**：`jsUint8ArrayOrNull`（`JZJSEngine.h`）—— 非 typed array 返回
   nullptr 且**不留异常**。直接 `JS_GetUint8Array` 探测会把 "not a TypedArray" 挂在 ctx 上，
   之后任何 quickjs 调用都变成抛错/返回 `JS_EXCEPTION`（`sqlite` / `core` / `net` / `serialport`
   都踩过）。
4. **所有权一直在 JS + 守卫防悬垂**：`JZObject` 派生类的 opaque 是 `JZObjectPointer*`（`guardedOpaque`），
   对象可能先被 C++（父控件）删掉 ⇒ 守卫认出"已销毁"就抛异常，JS 侧拿不到野指针，
   也天然防重复删除；`destory` 的语义是"**释放 opaque**"，不是"释放对象"。
   ⛔ 不要再引入 `jsOwned` 这套所有权翻转。
5. **借出去的视图用完要作废**：`disarmOpaque()`（栈上的事件对象等）—— 否则 JS 侧那份包装
   的终结会按守卫删对象。
6. **值语义 vs 指针语义**：默认值语义（`JZPoint`/`JZSize`/`JZRect`/`JZColor`/`JZImage` 别调
   `setPointerSemantics()`）；指针语义才登记身份表（"同底层对象 ⇒ 同 JS 对象"，`===` 才有意义）。
7. **`self()` 靠调用栈**：quickjs 没有"取当前 this"的 API ⇒ 引擎维护 `m_selfStack`，
   判据是**注册路径**（类实例方法/属性访问占帧；命名空间全局函数不占帧；裸 C 函数引擎不经手）。
8. **回调必须挂 GC 根**：`retainCallback()/releaseCallback()` —— quickjs 的循环 GC 会回收
   "引用计数 > 0 但从根不可达"的对象，而"JS 包装 → C++ 对象 → 回调槽 → 闭包 → JS 包装"正好成环；
   登记到全局数组里才不会被收掉（`__jz.net` 的 on/off 回调、控件回调槽都用它）。
9. **加形参/属性文法**：`def` / `defProperty` / `defData` 走引擎 magic 分发（能拿 `self()`）；
   需要 `this_val` 或原始 argv 的接口只能走 `def(name, JSCFunction*)`（引擎不经手、不占帧）。

---

## 9. 控件族（特例：不是模块）

控件**不是**"Node 内置模块"，所以走另一套约定（细节见 `widget/types/*.d.ts` 与 `widget/index.ts`）：

- 类**由引擎注册成全局类**，实例由 C++（`__JZWidgetObject` 那条路）创建；
  TS 侧只是"壳"：`class JZWidget extends __JZWidgetObject` + `super(nocreate)` + `init_c_xxx(this)`；
- 加载顺序固定：`classes`（定义全局类）→ C++ 挂方法到原型 → `tree/props/signals/menus`；
  `classes.ts` 必须**幂等**（重复加载不能重复定义）；
- 信号 `defSignalProperty` 在 C++ 只登记名字，原型由 `signals.ts` 装 ⇒ **两边成对加、缺一不可**；
- `setProperty/property` 是**纯 JS 侧**存储（`props.ts` 的 WeakMap），模型面永远不暴露
  item / 句柄这类 C++ 指针；
- `@jz/widget` 模块里的 `Widget` **不是真控件**，只是 `.ui` 代码生成用的"UI 树数据节点"（类型占位）；
- 运行期控件类是 `globalThis` 上的全局类；`@jz/widget` 只提供 TS 模块化入口。

---

## 10. 已知不一致 / 待办（诚实清单）

| # | 现象 | 影响 | 状态 |
| --- | --- | --- | --- |
| 1 | ~~ambient 声明了具名导出、桥接只给 default/namespace ⇒ `import { createServer } from 'http'` 运行崩~~ | — | ✅ 已修（2026-09-22 一名字一包：包出口本身就有具名成员，桥接同时给 `export *` + default） |
| 2 | 模板 `moduleResolution: bundler` ⇒ `allowSyntheticDefaultImports` 默认 true（"少 default"抓不到）—— 现在所有名字都有 default，实际影响已消失 | 小 | 待办（可选：`allowSyntheticDefaultImports: false`） |
| 3 | 裸名默认导入的类型依赖各包 default 声明（已补），但 strict 下仍需 `import type {...}` 拿具名类型 | 小 | 未修 |
| 4 | 没有"真全局 process"（`import process from 'process'` 才是模块；遗留全局 `process` 只有 `cwd`，且封装里**没有 `exit`**；`platform` 是属性不是函数） | 习惯写法必踩 | 未修（文档/提示词标注） |
| 5 | `serialport` 在 `code_runtime` 里不可用（`initEngine` 只注册 net/core/sqlite/modbus/widget/worker） | 运行期 TypeError | 评测集不含 serialport |
| 6 | `URL` 的 fragment 会并进 `search` 且最后一个参数不 percent-decode | 用到 `#` 就错 | 未修（规避） |
| 7 | `dgram` 类型缺 `setEncoding` ⇒ UDP 只能 `toHex()` | 形状不一致 | 未修 |
| 8 | tsgo(TS7) 不吃 ambient 里的通配再导出 | 部署侧靠 `package.py` 补丁兜住 | 已兜住（生成物） |

> 同族判据：**tsc 绿 ≠ 能跑**。凡是"类型声明的导出集 ≠ 桥接给的导出集"，就会出现在这张表里。

---

## 11. CheckList

### 加一个 Node 名字（= 加一个包目录）

1. 建目录 `js_wrapper/<名字>/{index.ts, types/index.d.ts, types/ambient.d.ts, package.json, tsconfig.json}`
   （`index.ts` 用 `export default <模块对象>` + 具名导出各成员；ambient 写
   `declare module '<名字>' { export * from './index'; export { default } from './index'; }`）；
   跨包 import 用 `'<依赖名字>/dist'`，并在自己 tsconfig 的 `paths` 里加一条映射；
2. `JZJSEngine.cpp` 的 `builtinModules[]` 加一行（file = `<名字>/dist/index.js`，exportName 一般 `nullptr`）；
3. `js_wrapper/tsconfig.json` 的 `references` 加 `{ "path": "./<名字>" }`（顺序在依赖之后）；
4. `js_wrapper/types/index.d.ts` 的内置名列表 + 提示词 `jzTsRuntimeNotes()`；
5. `tsc -b js_wrapper`（只改 `.d.ts` 也要跑），跑 `test/jsengine` 的相关测试；部署由
   code_agent 的 POST_BUILD（`package.py`）自动重打包。

### 加一个包（非内置名）

1. `js_wrapper/<pkg>/{package.json,tsconfig.json,index.ts,types/,dist/}`；
2. `__jz.<pkg>` 原子命名空间在 C++ 模块里注册（`registFunction` / `registAsyncFunction`）；
3. C++ 侧 `initEngine` 装配该模块（宿主可能还有 Web 全局 bootstrap 之类的接线）；
4. `package.py` 的 `find_packages()` 自动认"带 `types/` 或 `dist/` 的一级子目录"，无需登记；
5. 若是纯 JS 包，进 `test/jsengine` 加一个 `.ts` 用例。

### 验证口径（"tsc 绿 ≠ 能跑"）

- `tsc -b JZJSEngine/js_wrapper`：类型层；
- **真跑**：`code_runtime --entry <产物目录>/main.js` 跑一段用该 API 的小程序（`build/jsapi_ref.ps1` 的
  9 份参考实现是范例：每条用例都要求打印一行 `@` 连接的计算值，断言输入派生值，不可硬编码）；
- 引擎侧单测在 `test/jsengine/`（`test_jsnet` / `test_jssqlite` / `test_jsworker` / `test_jsui` ...）。
