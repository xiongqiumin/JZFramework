# JIT-Pixel-Op 需求与设计文档（V1.0 · TCC 阶段）

> 2026-10-15 成文并完善：原始需求稿 + 对着**本仓当前代码**补全的落地设计。
> 场景 = Computer-Use 图像自定义像素算子；宿主 = C++（QuickJS-ng + OpenCV）；阶段 1 = **libtcc(TinyCC)** 内存 JIT，标量 C99，无自动 SIMD。
> 参照文档：`js-engine-wrapper-design.md`（引擎封装/`__jz` 约定/加包 5 步）、`compute-use-design.md`（Computer-Use 分层）、
> `code-agent-design.md`（终端/工具纪律）。本文所有 API 名都按本仓 `JZJSEngine/quickjs/quickjs.h`（quickjs-ng）写，与需求稿不同的地方单独标注。

---

## 0. 一句话与三条铁律

**开发者用 TS/JS 写逐像素算子，`// @jit` 注解标记；首次调用时在进程内编译成原生机器码（libtcc），之后绕过 QuickJS 字节码解释器；任何一步不满足就无声降级回 JS，功能不失效。**

三条铁律（后面所有设计都从这三条推出来）：

| # | 铁律 | 推论 |
| --- | --- | --- |
| 1 | **降级必须"无声且正确"**：不进 JIT 时行为与纯 JS **逐像素一致** | 每个算子必须双路径（JIT/JS）对比单测；stats 可观测，否则是假 PASS |
| 2 | **原生码执行期间不碰 JS**：不分配、不调 JS 函数、不触发 GC | 裸 `uint8_t*` 在单次调用内恒定有效；子集禁止算子内调 JS 是硬约束不是偏好 |
| 3 | **存储语义跟随实参 view 类型**（`Uint8Array`=wrap / `Uint8ClampedArray`=clamp），编译期定、运行期按 kind 分发，不符即回退 JS | 同一算子对不同 view 类型编不同变体；绝不猜 |

---

## 1. 背景、目标与非目标

### 1.1 背景

Computer-Use 图像处理里 OpenCV 标准算子覆盖不到大量**小众、临时、启发式逐像素逻辑**（自定义掩码、特殊色彩变换、ROI 局部规则、变种模板过滤）：

- TS/JS 直写像素循环：QuickJS 字节码解释，大图性能差；
- 全部手写 C++ 扩展：改一个算子重编一次宿主，不利于业务快速调试、热更新；
- 不想引 LLVM/Clang 增加包体积 ⇒ 先做轻量可落地版本。

### 1.2 总体目标

1. TS/JS 写算子，注解标记启用 JIT；
2. 首次执行时进程内内存编译为原生机器码，后续调用直接执行原生码；
3. 编译失败 / 超出子集 ⇒ 自动降级回字节码解释，仅性能下降；
4. 像素 Buffer 零拷贝：复用 OpenCV `cv::Mat`（或 `JZImage`）内存与 QuickJS TypedArray；
5. 不做运行时热点采样，**静态注解**标记；
6. Phase-1 libtcc 输出标量 C；Phase-2 可平滑换 LLVM-ORC 拿自动向量化。

### 1.3 非目标（V1 不做）

1. ❌ 通用 JS 全语言 JIT（仅像素算子窄子集）；
2. ❌ 自动 SIMD（TCC 无）；
3. ❌ 算子内嵌套调用 JS 函数；
4. ❌ 替代 OpenCV 重型算子；
5. ❌ 运行时动态 eval JIT（必须加载阶段静态识别）；
6. ❌ （本仓补充）`while`/`do`、`%`/位运算、`Math.*`、字符串、对象、多返回值、生成器/async —— 见 §3 黑名单。

---

## 2. 整体架构与模块划分

```
TS 源码(.ts) ──tsc──▶ JS 产物(dist)
    │
    ▼  QuickJS 模块加载（js_module_loader → loadModuleDef）
JIT 源码钩子（仅文件模块、仅当宿主装了 JIT 模块）
    ├─ 扫描器：源码里找 "// @jit" + 下一个 "export function NAME("  → 记 NAME
    └─ 改写：模块源码**末尾**追加一行  NAME = __jz.jit(NAME);   （live binding 重赋值）
    ▼
模块求值：__jz.jit(fn) 造出 wrapper（CClosure），导出绑定指向 wrapper
    ▼
JS 调 toGray(view, w, h)
    ├─ 第 1 次：校验参数 → 取源码(toString) → 子集校验 → C codegen → libtcc 内存编译 → 存 native_fn
    ├─ 第 N 次：校验参数 → 直接调 native_fn（原生机器码）
    └─ 任一环失败：调 fallback 原始 JS 函数（永久回退，记日志）

标准图像逻辑：OpenCV C++ 接口（__jz.cv）      普通胶水逻辑：QuickJS 正常解释
```

### 2.1 模块划分（含本仓落点）

| 模块 | 职责 | 落点（本仓） |
| --- | --- | --- |
| `jit_ast_scanner` | JS 子集词法/语法/校验；从**模块源码**识别 `// @jit` 提取函数名；从**函数源码**（toString）解析算子体成 AST | `JZJit/JZJitAstScanner.{h,cpp}`（纯 C++，不依赖 quickjs） |
| `jit_c_codegen` | AST → 纯 C99 源码字符串；自动生成 GC 前提、钳位/wrap 辅助、TypedArray 裸指针提取、循环 | `JZJit/JZJitCCodegen.{h,cpp}` |
| `jit_tcc_driver` | 封装 libtcc：内存编译、符号预注册、relocate、取函数指针、编译互斥、错误收集 | `JZJit/JZJitTccDriver.{h,cpp}` + vendored `JZJit/tcc/` |
| `jit_wrapper` | QuickJS 包装层：分发（首调编译 / 次调原生 / 失败回退）、状态、引用计数、销毁 | `JZJit/JZJitWrapper.{h,cpp}` |
| （门面） | 算子注册表、编译调度、统计 | `JZJit/JZJit.{h,cpp}` + `JZJit/JZJit.cmake` |
| JS 绑定 | 注册 `__jz.jit`、安装模块源码钩子 | `JZJSEngine/JZJSJitModule.{h,cpp}`（源清单加进 `JZJit.cmake`，与 `JZJSComputerUseModule` 同规矩） |
| TS 侧包 | `import { jit } from 'jit'` + 算子类型声明 | `JZJSEngine/js_wrapper/jit/`（一个名字=一个包目录） |
| 单测 | 双路径逐像素对比 | `test/jsengine/test_jsjit.cpp` |

构建：`JZJit` 作为普通模块进 JZFramework（根 CMakeLists 模块列表 + `JZFramework.cmake` 的 `MODULE_DEPENDS_JZJit "JZCore"`）；
libtcc 在 `JZJit/tcc/` 编成静态库 `jz_tcc` 被 `JZJit` PRIVATE 链接（静态库符号不需要导出宏）。
宿主装配点 = `JZEngineRuntime::initEngine`（`binary/code_agent/JZEngineRuntime.cpp:152`，widget 之后、worker 之前，
与 `m_computerUseModule` 同一段）：`m_jitModule = std::make_unique<JZJSJitModule>(); m_jitModule->initEngine(engine);`。

> ⚠ **宿主不装 JIT 模块时代码零影响**：没有钩子就没有改写，`// @jit` 只是注释。这是刻意留的退路。

---

## 3. 算子开发约束（JIT 子集规范）

全部满足才进 JIT；任一不满足 ⇒ 静默 fallback（§9）。

### 3.1 JS 侧写法（两种标记，主/备）

**主（需求稿形态，注释 + 扫描器）**：

```ts
// @jit
export function toGray(buf: Uint8ClampedArray, w: number, h: number) {
    // 算子体（窄子集）
}
```

- 必须 `export function`（顶层函数声明）；
- 参数固定：`(buf, w, h)` —— buf 为 TypedArray 视图，w/h 为 number；
- 返回值 `void`（`return;` 可写可不写）；
- ⚠ **tsc 默认剥注释** ⇒ 工程 tsconfig 必须 `"removeComments": false`（`initProject` 模板已带，见
  `js-engine-wrapper-design.md` §7.2）。注释被剥 ⇒ 注解失效 ⇒ **不 JIT 也不报错**（降级路径）。

**备（工厂形态，任何情况都成立）**：

```ts
import { jit } from 'jit';
export const toGray = jit((buf: Uint8ClampedArray, w: number, h: number) => { ... });
```

用于：`export const`（不可变绑定，见 §4.3）、源码被压缩/打包、注释保留被关掉的工程。
`__jz.jit(fn)` 幂等：已是 wrapper 的原样返回。

### 3.2 语法白名单（允许）

| 类 | 内容 |
| --- | --- |
| 声明 | `let / const name = <number 表达式>;`（仅 number；`const` 不得再赋值） |
| 赋值 | `name = expr;`、`name += / -= / *= / /= expr;`、`name++ / name--`（前后缀） |
| 语句 | `for`、`if-else`、块 `{}`、`return;` |
| 表达式 | 算术 `+ - * /`；比较 `< <= > >= == !=`；逻辑 `&& \|\| !`；三元 `? :`；一元 `-` |
| TypedArray | 读 `buf[i]`、写 `buf[i] = expr` |
| 常量 | 数字字面量（十进制/十六进制）、`0/0`（NaN，可用于测试语义） |
| 标识符 | 三个形参 + 局部变量（无闭包捕获、无自由变量） |

### 3.3 黑名单（遇到即 fallback）

1. `any` 类型 / 普通 JS 对象属性读写 / 数组字面量 / 字符串；
2. 算子内部调用**任何** JS 函数（含 `Math.*`、回调、`this`）；
3. `eval / new Function`；
4. 闭包捕获外部逃逸变量（除三个形参外的一切外部标识符）；
5. 非 TypedArray 数组；
6. `try-catch / yield / async / while / do / switch / break / continue / label`；
7. `%`、位运算、`>>>`、`**`（V1 不留；候选扩充见 §15）；
8. 形参名不是恰好 3 个 / 形参带默认值/解构/剩余参数。

### 3.4 循环规范形态（静态可判定上界）

**只认 `for`**，且分两档：

- **规范循环**（走快路径）：`for (let i = <字面量>; i </<= <w|h|字面量>; i++)`（`+= 1` 等价）。
  校验器能静态证明"上界有限、步进为 1"⇒ 生成 `size_t` 计数器（§5.4）。
- **一般循环**：其它 `for` 形态（步进非 1、初值非字面量、复合条件）⇒ 按 `double` 计数器生成，语义保真但慢些。
- 条件里出现 `>= 0` 这类**不可静态判定上界**的形式（如倒计数、复合边界）⇒ 拒绝 JIT（fallback JS）。
  理由：原生码没有中断点（§12 风险表），一个死循环算子会卡住线程；JS 侧至少还能被引擎事件循环打断。

### 3.5 条件表达式规则

`if / for` 条件、`&&`/`||`/`!`/`?:` 的操作数**必须是比较运算或逻辑运算**（叶子是比较），
不允许裸数值真值（`if (x)`）—— JS 真值是 "0/NaN 为假"，C 里 NaN 为真，语义会分叉。
要写"非零即真"请显式 `x != 0`。

---

## 4. 接入点设计（低侵入 QuickJS，不改虚拟机内核）

### 4.1 模块源码钩子（新增引擎 API）

`JZJSEngine.h` 新增一个可选钩子，`loadModuleDef`（`JZJSEngine.cpp:3352`）在**文件模块**读到源码后、
`JS_Eval(... COMPILE_ONLY)` 之前调用：

```cpp
// module_name 已被归一化; code 可被就地改写; 返回 true = 改写过。
// 只对文件模块调(内置名桥接源码不调); 调用线程 = 加载模块的那个引擎线程。
using JZModuleSourceHook = std::function<bool(const std::string& module_name, std::string& code)>;
void setModuleSourceHook(JZModuleSourceHook hook);   // nullptr = 卸掉
```

`JZJSJitModule::initEngine` 安装它（每个引擎一份；worker 引擎各自装）。
钩子实现第一件事是 `code.find("@jit") == npos` 就返回 —— 对不含注解的模块零成本。

### 4.2 扫描器规则（V1）

1. 逐行找**独占一行**的注解：`// @jit` / `//@jit` / `/* @jit */`（前后空白不限）；
2. 下一行代码必须匹配 `export function NAME (`（NAME 为合法标识符）；
3. 同模块内同名只记第一个；不同模块互不影响；
4. 只认 `export function`。`export let`/`export const`/`import` 后再 `export {}` 一律不认（见 §4.3 的绑定可变性）⇒ 用工厂形态。

### 4.3 源码改写（末尾追加，不做 Span 匹配）

对命中的每个 NAME，在**模块源码末尾**追加（保证在函数声明之后求值）：

```js
/* jit-pixel-op: auto-wrapped */
toGray = __jz.jit(toGray);
```

依据：`export function` 产生的是**可变绑定**（var 语义），模块内重赋值即更新导出（ESM live binding）。
这比"改写声明处/ Span 匹配花括号"简单得多，且不影响函数自身的 `toString` 源码。

守卫（缺一即跳过改写、只记日志，绝不抛）：

- 注解形态不是 §4.2 认的 `export function`；
- 模块源码里已出现 `__jz.jit(`（用户手写过工厂 ⇒ 不重复包）；
- 写入后模块**编译失败** ⇒ 回滚：重新按原源码加载一次（或本次不 JIT），并记 `LOG_W`。
  （改写只在内存 `code` 上做，文件不落盘；每次加载都从文件重新扫描 ⇒ 天然幂等。）

### 4.4 算子源码获取（不自己切源码）

`__jz.jit(fn)` 里 `JS_ToString(ctx, fn)` 即得函数**源码原文**：quickjs-ng 解析时把函数源码存进
`JSFunctionBytecode.source`（`quickjs.c:36979` 保存、`36268` 挂上、`41796` 由 `Function.prototype.toString`
返回；字节码序列化/反序列化也带这段，`quickjs.c:39081`）。
所以扫描器只负责"**哪些函数要包**"，codegen 的输入来自 toString —— 两边不共享 Span，不会漂移。

### 4.5 wrapper 创建

```cpp
// JZJitWrapper（每个算子一份，堆对象，被 CClosure 的 opaque 持有）
struct JitWrapper {
    JZJSEngine* engine;
    JSValue      fallback;      // 原始 JS 函数（dup；经 engine->retainCallback 挂 GC 根，跨调用存活）
    int          fallbackRootId;
    std::string  name;          // 算子名（日志/统计）
    std::string  cSource;       // 生成的 C 源码（诊断用）
    // 编译状态（每个 view kind 一份；V1 kind: 0=Uint8ClampedArray, 1=Uint8Array）
    struct Variant { bool compiled; JitNativeFunc fn; TCCState* tcc; std::string error; };
    Variant      variants[2];
    std::mutex   mtx;           // 本 wrapper 的编译互斥（防同算子并发首调重复编译）
    std::atomic<int> refcnt;    // 正在执行原生码的调用数（销毁保护，见 §4.7）
    bool         gaveUp;        // 编译失败过 ⇒ 永久 JS
};
```

对外 JS 对象 = `JS_NewCClosure(ctx, jitDispatch, name, jitWrapperFinalizer, 3, 0, wrapper)`
（`quickjs.h:1298`；`JSCClosure` 签名 `quickjs.h:449`）。挂一个不可枚举属性 `__jz_jit_wrapper__ = true`
供幂等判断。fallback 的 GC 根用引擎现成机制 `JZJSEngine::retainCallback()`（`JZJSEngine.h:525`，
与 net/控件回调同一份根数组）—— quickjs-ng **没有** `JS_AddValueRoot`（需求稿 §4.2 的写法不适用），
且"只被 C++ 持有的 JS 函数"必须挂全局根数组才不会被循环 GC 收掉。

### 4.6 分发逻辑

```
dispatch(ctx, this, argc, argv):
  w = wrapper
  if (!w) return JS_UNDEFINED                      // 已被销毁(不该发生)
  kind = viewKind(argv[0])                         // JS_GetTypedArrayType: 0=UINT8C / 1=UINT8 / -1=其它
  if (argc < 3 || kind < 0
      || !JS_IsNumber(argv[1]) || !JS_IsNumber(argv[2])
      || w/h 越界(§5.5) || 字节数 < w*h*4 || ctx 有待决异常):
      return callFallback(w, argc, argv)           // 参数形状不符 ⇒ 本次走 JS(保语义)
  v = &w->variants[kind]
  if (!v->compiled):
      lock(w->mtx)
      if (!v->compiled && !w->gaveUp):
          ok = compileOperator(w, kind)            // toString → 子集校验 → codegen → tcc
          if (!ok) { w->gaveUp = true; log; unlock; return callFallback(...) }
          v->compiled = true
      unlock
  if (!v->compiled) return callFallback(...)
  w->refcnt++ ; r = v->fn(ctx, this, argc, argv) ; w->refcnt--
  return r
```

要点：

- **首次调用触发编译**（需求稿语义）；另留开关 `JZ_JIT_EAGER=1` 在 `__jz.jit(fn)` 时就编译（基准测试用）。
- 编译失败 ⇒ `gaveUp`，之后**永远** JS；只记一次 `LOG_W`（含算子名 + tcc 错误首行）。
- `callFallback` = `JS_Call(ctx, w->fallback, this_val, argc, argv)`。此刻已在 JS 栈上（native 被 JS 调用），
  直接 `JS_Call` 安全（不涉及 job 泵问题；算子本身同步）。
- 参数校验放在**分发层**而不是生成码里 ⇒ 生成码的提取 shim 不可能失败（§5.5）。

### 4.7 热更新与销毁

- 算子源码重新加载 = 新模块求值 = 新 wrapper；旧 wrapper 的 CClosure 失去 JS 引用后被 GC ⇒ finalizer：
  `refcnt--`；若 `refcnt == 0` 立即 `tcc_delete`（释放代码页）+ `releaseCallback` + delete；否则挂到
  "待删表"，由最后一个执行中的调用在退出时删。**没有引用计数的直接 delete 会让正在另一线程执行的原生码
  踩已释放页** —— 这是 JIT 代码生命周期的经典坑，V1 就用引用计数挡掉。
- 引擎 `deinitJSEnv` 前统一 reap：所有 wrapper 强制作废（记 LOG_W 若有仍在执行的）。

---

## 5. C 代码生成规范（jit_c_codegen）

生成**纯 C99**，`#include "quickjs.h"`（quickjs.h 只依赖 `stdbool/stdio/stdint/string/math`
（`quickjs.h:30-34`），全在 TCC 自带 include 里；TCC 侧 `tcc_add_include_path(<repo>/JZJSEngine/quickjs)`）。
生成码只 `extern` 两个宿主 shim（下表），其余全部自足 —— 符号面越小，`tcc_add_symbol` 漏注册的概率越低。

### 5.1 生成函数骨架

```c
/* ===== JIT-Pixel-Op 自动生成: toGray (view kind: Uint8ClampedArray) ===== */
#include "quickjs.h"

/* 宿主 shim(jit_tcc_driver 用 tcc_add_symbol 预注册; 分发层已校验参数, 这些调用不会失败) */
extern const uint8_t *jzJitGetPixels(JSContext *ctx, JSValueConst v, size_t *n);
extern int             jzJitToI32   (JSContext *ctx, JSValueConst v);

static uint8_t clamp_u8(double v) { /* §5.2 */ }

JSValue js_toGray(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    size_t n = 0;                                       /* 元素数(=字节数, u8) */
    const uint8_t *pixels = jzJitGetPixels(ctx, argv[0], &n);
    const size_t w = (size_t)jzJitToI32(ctx, argv[1]);
    const size_t h = (size_t)jzJitToI32(ctx, argv[2]);

    /* ===== 算子体(AST 翻译) ===== */
    size_t y, x;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            size_t i = (y * w + x) * 4;
            double r = pixels[i];
            double g = pixels[i + 1];
            double b = pixels[i + 2];
            double v = (r + g + b) / 3.0;
            pixels[i]     = clamp_u8(v);
            pixels[i + 1] = clamp_u8(v);
            pixels[i + 2] = clamp_u8(v);
        }
    }
    /* =========================== */

    return JS_UNDEFINED;   /* 常量宏, 无需符号 */
}
```

`this_val` 忽略（算子是普通函数）；`argc` 不再判断（分发层已判）。

**GC 前提（替代需求稿的 AddValueRoot）**：从取到 `pixels` 到循环结束，生成码**不调用任何宿主 API**
（shim 只在入口调用两次）⇒ 不分配 ⇒ quickjs GC 不可能在此期间跑 ⇒ `argv[0]` 由 JS 栈帧保根、
裸指针恒定有效。这是铁律 2 的直接兑现，也是"子集禁止算子内调 JS"的根本原因。

### 5.2 存储语义辅助函数（需求稿的 clamp_u8 有两处语义错，已修）

`Uint8ClampedArray` 存储语义（`ToUint8Clamp`）不是"钳位 + 截断"，而是 **NaN→0；越界钳 [0,255]；
round-half-to-even（银行家舍入）**。截断写法会把 `1.5→1`（应为 2）、`NaN→UB`：

```c
static uint8_t clamp_u8(double v)
{
    if (isnan(v)) return 0;                 /* ToNumber(NaN) -> 0 */
    if (v <= 0.0) return 0;                 /* 含 -0 */
    if (v >= 255.0) return 255;
    double f = floor(v), d = v - f;
    if (d > 0.5) f += 1.0;                  /* 常规进位 */
    else if (d == 0.5) f = (fmod(f, 2.0) == 0.0) ? f : f + 1.0;  /* 恰好一半取偶数 */
    return (uint8_t)f;
}
```

plain `Uint8Array` 视图（kind=1）换 `wrap_u8`（`ToUint8`：NaN/±0/±∞→0，其余截断取模 256）：

```c
static uint8_t wrap_u8(double v)
{
    if (isnan(v) || v == 0.0 || isinf(v)) return 0;
    double m = fmod(trunc(v), 256.0);
    if (m < 0.0) m += 256.0;
    return (uint8_t)m;
}
```

**读**两侧一致：`pixels[i]` 提升为 double，与 JS 读 TypedArray 得 number 完全同值。

### 5.3 表达式/语句翻译规则

| JS | C | 说明 |
| --- | --- | --- |
| 局部 number 变量 | `double`（块作用域声明，位置与 JS 一致） | JS number 即 double；禁整数化以免语义分叉 |
| `a / b` | `a / b`（double） | 除零得 ±Inf/NaN，与 JS 一致 |
| `a == b` / `!=` | 同 | NaN != NaN 两侧一致 |
| `&& / \|\| / ! / ?:` | 同（C 短路一致） | 叶子是比较（§3.5） |
| `buf[i]` 读 | `pixels[(size_t)i]` | |
| `buf[i] = e` 写 | `pixels[(size_t)i] = clamp_u8(e)`（或 `wrap_u8`） | 唯一插入语义函数的地方 |
| `i++` / `i += 1` | 同 | 一般循环里是 double 自增 |

### 5.4 规范循环 → `size_t`

§3.4 的规范循环生成无符号计数器（比 double 比较快，且可被 TCC 更好地寄存器分配）：

```c
for (size_t y = 0; y < h; y++) { ... }     /* 上界 w/h/字面量直接拿来用 */
```

非规范循环退回 `for (double i = ...; ...; ...)`。索引统一 `(size_t)` 强转 —— V1 约定算子下标合法（§5.6）。

### 5.5 入口校验（分发层做，生成码不重复做）

| 校验 | 不满足的处理 |
| --- | --- |
| `argc >= 3`、`argv[0]` 是 TypedArray 且 kind ∈ {UINT8C, UINT8}、`argv[1..2]` 是 number | 本次调用走 JS（JS 会按自己的语义抛错/空转，逐像素一致） |
| `0 <= w,h <= kMaxDim`（`kMaxDim = 1<<26`，防 `w*h*4` 溢出 size_t 的极端输入） | 走 JS |
| `n >= w*h*4`（视图装得下算子要写的范围） | 走 JS（**不抛错**：JS 语义下越界写是 no-op、越界读是 undefined，跑 C 会踩内存） |
| ctx 无待决异常 | 走 JS |

### 5.6 边界检查策略

- **生产构建**：循环内**不生成**越界检查（需求稿决定，换性能）；正确性靠 §11 单测 + §3 子集收窄。
- **单测/CI 构建**：宏 `JZ_JIT_BOUNDS_CHECK` 让 codegen 给每次读写套一层检查 helper
  （OOB 读→0、OOB 写→no-op，对齐 JS），用来验证"无检查版与 JS 版逐像素一致"这个论断本身。
  两版同源，只差一个宏 ⇒ 不存在"测了一套、发另一套"。

---

## 6. libtcc 集成设计（jit_tcc_driver）

### 6.1 编译流程（全内存，不落盘）

```c
TCCState* tcc = tcc_new();
tcc_set_error_func(tcc, &errSink, onTccError);          // 错误收进 std::string(首行即报告)
tcc_set_output_type(tcc, TCC_OUTPUT_MEMORY);
tcc_add_include_path(tcc, "<repo>/JZJSEngine/quickjs"); // 供 #include "quickjs.h"
tcc_add_symbol(tcc, "jzJitGetPixels", (const void*)jzJitGetPixels);
tcc_add_symbol(tcc, "jzJitToI32",    (const void*)jzJitToI32);
/* clamp_u8/wrap_u8 是生成码里的 static 函数, 不需要注册 */
tcc_compile_string(tcc, csrc);        // 失败: 错误文本已由 error_func 收齐
tcc_relocate(tcc, TCC_RELOCATE_AUTO); // libtcc 自行分配并管理可执行内存页
void* fn = tcc_get_symbol(tcc, "js_toGray");
```

对应需求稿 §5.1 的两处修正：

1. **`tcc_relocate(tcc, TCC_RELOCATE_AUTO)`**（不是 `tcc_relocate(tcc, NULL)`）。`NULL` 在新版 libtcc 的语义是
   "只返回所需内存大小、不落地"，`TCC_RELOCATE_AUTO` 才是"libtcc 自己分配并管理可执行内存"
   （Linux mmap RX / Windows VirtualAlloc，平台差异 libtcc 内部处理）。
   ⚠ libtcc **跨版本 API 形态不同**（老版 `tcc_relocate(TCCState*)` 无第二参数）：驱动层把这步包成
   `jitTccRelocate()` 一个函数，vendored 版本定死；接新版本只改这一处。
   若某平台 libtcc 拿不到 RX 页 ⇒ 走 §9 降级（日志 + JS）。
2. **TCCState 不能随手 delete**（修正需求稿 §5.1 第 8 条）：`TCC_RELOCATE_AUTO` 的代码页由 TCCState 管理，
   `tcc_delete` 会连代码页一起释放。⇒ V1 策略：**TCCState 活到 wrapper 销毁**（§4.7），
   "编译完即删上下文"只在两段式自管内存（`relocate(NULL)` 取 size → 自部分配 RX 页 → `relocate(buf)`）时成立，
   作为省内存的可选项留在驱动层，默认不用。

### 6.2 符号预注册

生成码只引两个宿主 shim（`jzJitGetPixels` / `jzJitToI32`，实现在 `JZJitTccDriver.cpp`）：

| shim | 实现 | 纪律 |
| --- | --- | --- |
| `jzJitGetPixels(ctx, v, n)` | 先 `JS_GetTypedArrayType`（不抛）判 kind，再 `JS_GetUint8Array`；返回 `abuf->data + ta->offset`（`quickjs.c:58572`，**已含 byteOffset**）；失败返回 NULL | 与 `jzUint8ArrayOrNull`（`JZJSEngine.h:783`）同纪律：**不留待决异常**（探测型 API 铁律，jsbind 踩过）；万一 `JS_GetUint8Array` 抛了（OOB/detach），吞掉异常返 NULL |
| `jzJitToI32(ctx, v)` | `JS_ToInt32`（number 已由分发层校验，不会抛） | |

Windows DLL 构建（`JZFRAMEWORK_DYNAMIC=ON`）下 `JS_GetUint8Array` 等 quickjs 符号由
`WINDOWS_EXPORT_ALL_SYMBOLS` 自动导出 ⇒ shim 里调它们没问题；生成码侧反而是**显式传函数地址**给
`tcc_add_symbol`，不依赖动态查找，静态/DLL 构建都成立。

### 6.3 线程安全

1. **编译阶段全局互斥**：进程级 `std::mutex g_tcc_mutex` 护住 `tcc_compile_string/relocate/get_symbol`
   （TCCState 非线程安全；同一时刻只编一个算子）；
2. **编译产物无锁**：原生码只读自己的代码页、只写调用方 buffer，无共享可变状态 ⇒ 多线程/多引擎并发调用安全；
3. 每 wrapper 另有自己的 `mtx` 防"同算子并发首调编两遍"（§4.6 双检）；
4. libtcc  vendored 位置 `JZJit/tcc/`，编成静态库 `jz_tcc`（PRIVATE 链进 JZJit）；文件清单与构建口径
   （`-DTCC_TARGET_X86_64` / `-DTCC_TARGET_ARM64`，Windows 另含 `tccpe.c`）以 vendored 版本的说明为准 —— 
   ⚠ MSVC 编 tinycc 需要小补丁，**优先预编译 libtcc.lib（x86_64/AArch64 各一份）随仓带**，CI 双架构验证。

---

## 7. JIT Wrapper 分发层

数据结构与调用逻辑见 §4.5/§4.6（需求稿 §6 的 `JitWrapper` 已按 quickjs-ng 现实改写：
`JSValue fallback` + `retainCallback` 根替 `JS_AddValueRoot`；`std::mutex*` 改值成员；
新增 `variants[2]`、`refcnt`、`gaveUp`）。对外语义不变：

1. 第一次进入（未编译）⇒ 加锁走全流程；成功存 `native_fn`；失败置 `gaveUp` 永久 JS；
2. 后续 ⇒ 直接调原生指针；
3. JS 侧完全无感：wrapper 就是个普通函数对象（`length=3`、`name=算子名`）。

---

## 8. 内存模型（Computer-Use 图像）

### 8.1 谁持有像素

- `cv::Mat`（或 `JZImage`，RGBA8888 紧凑）是像素内存持有者；
- JS 拿到的是**外部 buffer 上的 `Uint8ClampedArray` 视图**，**不拥有**底层内存（生命周期归 Mat）；
- 视图创建（新宿主 API，放 `__jz.cv`：`cv.view(id) -> {buf, width, height}`，因为 Mat 句柄表在
  `JZJSCvModule` 里；细节照 `JZJSCvModule.h` 的句柄纪律）：

```cpp
// 1) 外部 ArrayBuffer: free_func=nullptr ⇒ GC 时只释放 JS 包装, 不碰 Mat 内存
//    (js_array_buffer_finalizer: "if (abuf->free_func) ..." —— quickjs.c:57937)
JSValue ab = JS_NewArrayBuffer(ctx, (uint8_t*)mat.data, total, nullptr, nullptr, false);
// 2) 禁 detach/resize ⇒ 原生码手里的裸指针在整个 Mat 生命周期稳定
JS_SetImmutableArrayBuffer(ctx, ab, true);           // quickjs.h:1061
// 3) 在它上面建 Uint8ClampedArray 视图(构造器 (buffer, byteOffset, length) 形态,
//    与 quickjs 字节码 reader 同一路径 quickjs.c:39333-39341)
JSValueConst args[3] = { ab, JS_MKVAL(JS_TAG_INT, 0), JS_MKVAL(JS_TAG_INT, (int)total) };
JSValue view = JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_UINT8C);  // quickjs.h:1079
JS_FreeValue(ctx, ab);
```

⚠ 两个必须处理的 Mat 细节：

- **行距**：`mat.step != cols*elem` 时不能直接当紧凑缓冲 —— view 前先取紧凑副本
  （`cv::Mat compact = mat.isContinuous() ? mat : mat.clone()`），视图只暴露紧凑内存；
- **生命周期**：view 的 opaque 里 `cv::Mat` **按值持一份**（Mat 是引用计数共享数据）⇒ 即使
  `cv.release(id)` 把句柄表里的条目放了，视图还活着时数据页就不会被释放（杜绝 UAF）。

### 8.2 GC 与裸指针规则

- GC 只可能回收 JS 包装对象；底层 Mat 内存不归 QuickJS 管；
- 原生码拿到的裸指针**仅在单次调用内有效**（铁律 2 保证期间无 GC/无 detach）；
- 禁止 JIT 代码/ wrapper 把裸指针存进任何跨调用结构（wrapper 只存 JS 值 + 函数指针）。

---

## 9. 降级与可靠性策略

**所有降级路径功能可用、仅性能下降、不崩溃。** 触发清单（任一条即回退 JS，默认只记 `LOG_W`/统计，不抛）：

| 触发 | 层级 |
| --- | --- |
| 模块源码钩子没装 / 注解形态不认 / 模块编译失败回滚 | 加载期（等于不 JIT） |
| `// @jit` 被 tsc 剥掉（`removeComments` 没关） | 加载期（静默） |
| toString 拿不到源码（native 函数/ bind 出来的） | `__jz.jit(fn)` 时 |
| 子集校验不过（§3 黑名单/白名单外语法） | `__jz.jit(fn)` 时 |
| codegen/tcc 编译错误、符号缺失、RX 页分配失败 | 首次调用 |
| 参数形状不符、尺寸不符、view kind 未编译变体且编译失败 | 每次调用 |
| 小图（`w*h < JZ_JIT_MIN_PIXELS`，默认 4096） | 每次调用（逻辑上仍可 JIT，业务侧 hint 关掉；统计里分开记） |

**可观测性**：`__jz.jit.stats()` → `{operators, compiled, gaveUp, nativeCalls, jsCalls, lastError, compileMsTotal}`；
单测必须断言 `compiled>0 && nativeCalls>0` 才算真跑了 JIT 路径（benchmark"反例探针"纪律：
只跑正例的 PASS 不算数）。

---

## 10. 性能预估（V1-TCC 标量）

基准 = QuickJS 字节码解释执行同一算子：

1. 计算密集（卷积、复杂色彩变换）：**30-50 倍**；
2. 带宽受限（灰度、简单阈值）：**10-20 倍**（撞内存带宽墙）；
3. 限制：一次性编译开销（libtcc 编一个小算子 ≈ 毫秒级）被极小图摊薄；内层调 JS 函数会打回原形
   （子集直接禁）；
4. Phase-2 换 LLVM-ORC + 自动向量化，标量基础上再 **2-4 倍**。

产品评估按保守 **20 倍**做预期。实测方法（本仓）：`code_runtime` 起一个基准脚本，同一算子分别走
`__jz.jit` 包装/不包装两条路，对 640×480 / 1920×1080 两档各跑 N 次取中位数（参考 `test/game/bench`
的 min-of-N 纪律：单次测量机器噪声能差数倍）。

---

## 11. 单元测试要求

落在 `test/jsengine/test_jsjit.cpp`（`add_executable` + `add_test(NAME jsengine.test_jsjit ... TIMEOUT 300)`，
风格照 `test_jsnet.cpp` 的 `check()/[PASS]/[FAIL]` 计数）：

1. **双路径逐像素对比**（需求稿 §10）：每个算子同时跑 JIT 版（`__jz.jit` 包装）与 JS 版（同源码不包装），
   整块 buffer 比对，返回都是 undefined；
2. **case 覆盖**：正常图、`w=1`/`h=1` 边界宽高、极值像素 (0,255)、NaN 通道（`0/0` 写进去）、
   wrap 语义（300→44、-1→255）、半偶数（1.5→2、2.5→2 —— 专抓 clamp/截断分叉）；
3. **JIT 真生效断言**：`stats().compiled>0 && nativeCalls>0`；
4. **fallback 用例**：坏算子（调 JS 函数 / 字符串 / 对象属性 / `while`）⇒ 不崩、结果与纯 JS 一致、
   `compiled==0`、`gaveUp==1`；
5. **边界检查版对照**：`JZ_JIT_BOUNDS_CHECK` 宏编一份，验证无检查版与 JS 版一致（§5.6）；
6. **GC 压测**：反复调算子的同时循环制造垃圾触发 GC（验证 wrapper/fallback 根、无 UAF）；
7. **并发**：两个引擎（线程）同时首调不同算子 + 同算子（验证编译互斥与双检）；
8. **热更新**：重新加载模块 ⇒ 新 wrapper 编译成功、旧 wrapper 被 GC 后代码页释放（跑 valgrind/ASAN 变体，
   本仓有 `build-asan/`）；
9. TS 冒烟：`js_wrapper/jit` 的类型与一个 import 用例（tsc 绿 + `code_runtime` 真跑一行）。

---

## 12. 风险清单

| 风险 | 说明 | 应对 |
| --- | --- | --- |
| RX 可执行内存权限 | 嵌入式 Linux/seccomp 下 mprotect RX 失败；Windows 需 VirtualAlloc（libtcc 内部处理） | 失败即日志 + fallback；部署环境提前评估安全策略 |
| libtcc 版本 API 差异 | `tcc_relocate` 签名/语义跨版本不同（老版无 ptr 参数、无 `TCC_RELOCATE_AUTO`） | vendored 定死版本；驱动层 `jitTccRelocate()` 单点封装；CI 双架构 |
| MSVC 编 libtcc | tinycc 是 GNU C，MSVC 需补丁 | 优先随仓带预编译 libtcc.lib（x86_64/AArch64） |
| 源码改写出意外 | 末尾追加语句破坏模块（如绑定不可变、编译失败） | 只认 `export function`；失败回滚 + `LOG_W`；单测覆盖 |
| 原生码执行期 wrapper 被 GC | 另一线程正在跑原生码时代码页被释放 ⇒ UAF | `refcnt` + finalizer 延迟删除（§4.7） |
| JS/C 语义不一致 | `ToUint8Clamp` 的 NaN/round-half-to-even、`ToUint8` 的 wrap、double 比较 | codegen 语义函数（§5.2）+ 逐像素 diff 单测 + 边界检查版对照 |
| 无边界检查踩内存 | 算子下标越界 ⇒ 堆破坏 | 入口尺寸校验（§5.5）+ 子集收窄 + 单测；`JZ_JIT_BOUNDS_CHECK` 兜验证 |
| 死循环算子卡死线程 | 原生码无中断点，`engine.isInterruption()` 进不到原生码里 | 循环形态静态校验（§3.4）+ 尺寸上限；真卡死只能杀进程（保留现场再杀） |
| TCC 无自动向量化 | 大图循环拿不到 SIMD | V1 接受；Phase-2 LLVM-ORC |
| 架构兼容性 | ARM/RK 交叉编译 libtcc | 预编对应架构 libtcc.a；CI 验 x86_64/AArch64 |

---

## 13. 演进路线

### Phase-1（当前，TCC）

AST 扫描 + 子集校验、C codegen、libtcc 内存 JIT、wrapper 分发、降级逻辑、像素单测。
产出：可工作原型，标量原生像素算子。

### Phase-2（换后端）

保留 AST IR（`jit_ast_scanner` 的输出即稳定接口：子集校验与 wrapper 上层不动），
把 `jit_c_codegen` 换成 LLVM-IR codegen（或继续出 C 交给 Clang），开循环优化 + 自动向量化。
驱动层接口（`compileOperator(source) -> fn`）不变 ⇒ 换后端 = 换一个 `jit_tcc_driver` 的实现。

### 本仓落地清单（按依赖顺序）

1. `JZJit/` 目录 + `JZJit.cmake`（scanner → codegen → tcc driver → wrapper → 门面）+ vendored `JZJit/tcc/`；
2. 根 `CMakeLists.txt` 模块列表加 `JZJit`；`JZFramework.cmake` 加 `MODULE_DEPENDS_JZJit "JZCore"`；
3. `JZJSEngine.h/.cpp`：`setModuleSourceHook` + `loadModuleDef` 调用点（§4.1）；
4. `JZJSEngine/JZJSJitModule.{h,cpp}`：`__jz.jit`（`wrap`/`stats`）+ 钩子安装；
5. `binary/code_agent/JZEngineRuntime.cpp` `initEngine` 挂载（§2.1）；
6. `js_wrapper/jit/` 包（`index.ts` + `types/` + ambient）+ `builtinModules[]` 加 `{ "jit", "jit/dist/index.js", nullptr }`
   + `types/index.d.ts` 清单 + `jzTsRuntimeNotes()`（加名 5 步，见 `js-engine-wrapper-design.md` §2.1）；
7. `initProject` 的 tsconfig 模板确认 `"removeComments": false`；
8. `__jz.cv.view(id)`（§8.1）；
9. `test/jsengine/test_jsjit.cpp` + CMake/ctest；
10. 编译：Debug 版 agent_cli/code_runtime 不受影响（钩子未装=零影响）；全量 Debug 0 warning 后跑
    `ctest -C Debug -R jsengine`。

### CheckList

**加一个算子**：写函数（`// @jit` 或 `jit(...)`）→ 确认 tsconfig 留注释 → 跑 → `__jz.jit.stats()` 看 compiled。
**加一条子集语法**：白名单（§3.2）→ scanner 语法 → IR → codegen → **补双路径单测（含边界值）** → 更新 §3 表。
**换 JIT 后端**：实现 `compileOperator` → 动 driver 一层 → 全量单测 + 基准。

---

## 14. 对外 JS 使用示例

```ts
// filters.ts —— 与普通 TS 模块无异, 只是加了注解
// @jit
export function toGray(buf: Uint8ClampedArray, w: number, h: number) {
    for (let y = 0; y < h; y++) {
        for (let x = 0; x < w; x++) {
            const i = (y * w + x) * 4;
            const v = (buf[i] + buf[i + 1] + buf[i + 2]) / 3;
            buf[i] = v; buf[i + 1] = v; buf[i + 2] = v;
        }
    }
}

// @jit
export function threshold(buf: Uint8ClampedArray, w: number, h: number) {
    for (let y = 0; y < h; y++) {
        for (let x = 0; x < w; x++) {
            const i = (y * w + x) * 4;
            const v = buf[i] > 128 ? 255 : 0;      // 三元(白名单内)
            buf[i] = v; buf[i + 1] = v; buf[i + 2] = v;
        }
    }
}
```

```ts
// main.ts
import { toGray, threshold } from './filters';
import cv from 'cv';

const id = cv.imread('shot.png');                 // 或 computeruse.screenshotImage() → cv.fromImage
const view = cv.view(id);                          // { buf: Uint8ClampedArray, width, height } 零拷贝
toGray(view.buf, view.width, view.height);         // 调用方式与普通函数完全一致; 首次调用编译, 之后原生码
threshold(view.buf, view.width, view.height);
cv.imwrite(id, 'out.png');                         // 算子写的就是 Mat 本体
console.log((globalThis as any).__jz.jit.stats()); // { compiled: 2, nativeCalls: 2, ... }
```

工厂形态（`export const` / 注释被剥时）：

```ts
import { jit } from 'jit';
export const invert = jit((buf: Uint8ClampedArray, w: number, h: number) => {
    for (let y = 0; y < h; y++)
        for (let x = 0; x < w; x++) {
            const i = (y * w + x) * 4;
            buf[i] = 255 - buf[i]; buf[i + 1] = 255 - buf[i + 1]; buf[i + 2] = 255 - buf[i + 2];
        }
});
```

---

## 15. 已知不一致 / 待办（诚实清单）

| # | 项 | 状态 |
| --- | --- | --- |
| 1 | 需求稿 §4.2 的 `JS_AddValueRoot/JS_RemoveValueRoot/JS_GetTypedArrayData` 在本仓 quickjs-ng 不存在 ⇒ 已按现实改写（GC 前提 = 原生期不分配；根 = `retainCallback`；取指针 = `JS_GetUint8Array`） | ✅ 本文已改 |
| 2 | 需求稿 §4.1 的 `clamp_u8` 缺 NaN 处理且用截断 ⇒ 与 `Uint8ClampedArray`（round-half-to-even）不符 | ✅ §5.2 已修 |
| 3 | 需求稿 §5.1 第 6/8 条：`tcc_relocate(tcc, NULL)` 语义、删上下文保留代码页 ⇒ 与新版 libtcc（`TCC_RELOCATE_AUTO` 由 TCCState 管内存）不符 | ✅ §6.1 已改 |
| 4 | 需求稿 §3.2 白名单没写 `++`/`+=`/比较/逻辑/三元，但 §13 示例用了 `x++` | ✅ §3.2 已补齐 |
| 5 | 需求稿未覆盖：tsc 剥注释、ESM 导出绑定不可变（`export const` 不能靠末尾重赋值接管） | ✅ §3.1/§4.3（工厂形态兜底） |
| 6 | `cv.view()` 宿主 API 不存在（当前 `__jz.cv` 是句柄制，JS 拿不到像素） | 待实现（§8.1 已给设计） |
| 7 | `JZImage` → 视图直连（跳过 Mat 拷贝） | 待实现（V1.1；先走 `cv.fromImage` + `cv.view`） |
| 8 | 候选语法扩充：`%`、`Math.min/max/floor/abs` 白名单化、`while`（带上界静态证明） | V1.1 评估 |
| 9 | `eval()`/`evalScript` 路径的注解识别（V1 只走 ESM 模块 loader） | 待办 |
| 10 | AArch64 预编译 libtcc + CI | 待办 |
