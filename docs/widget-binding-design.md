# Widget 绑定方式的选择（引擎注册类 / 透明类 / 纯 C 函数）

> 2026-09-23 首次成文。把"控件族为什么不走 `registClass`、类定义放在 JS 侧、C++ 只出函数"
> 这条决定写成判据，并对照**当前代码**给出实现现状、代价清单与迁移路径。
> 参照文档：`js-engine-wrapper-design.md`（模块/包/名字的总原则）、`ai-native-design.md`。
>
> **结论先行**：控件族（`JZWidget` 及其 25 个派生类）一律 ——
> ① 类定义在 **JS** 侧（`js_wrapper/widget/classes.ts`）；
> ② JS 构造 = 造一份 **C++ 指针**（`init_c_xxx` 里的 `new`，写进 JS 对象的 op 槽）；
> ③ 控件类型对引擎 **透明**，不 `registClass`、不进 `m_typeMap`、没有 ctypeid；
> ④ C++ 侧只提供**函数**（`init_c_xxx` + 原型方法），不描述"类"；
> ⑤ `jsbind::ClassBind<C>` **保留**，但在控件侧它的角色只是**函数收集器**（收 `def(...)` 与
>    `setCreate(...)`），**不承担类语义** —— 见 §10。这是**刻意选择、不是遗留物，不需要改**。
> 值类型与引擎内部对象（`JZPoint`/`JZRect`/`JZColor`/`JZFont`/事件/布局/画笔/`sqlite`/`net` 的类）
> 走的仍是注册路径，两者分工见 §3。

---

## 1. 为什么控件族不适合"引擎注册类"

注册路径（`jsbind::ClassBind` + `JZJSEngine::registClass`）的设计目标是
"把一个 C++ 类原样搬进 JS 类型系统"：建类、建原型、建**形参表**、登记 ctypeid 与继承链、
进 `m_typeMap` / `m_jsTypeMap`，之后 `fromJSValue<T>` / `js_check_class_arg<T>` /
`isInstanceOf` / `toJsValue` 这一整套都认它。

控件族用不上这套，代价却不小：

- 引擎的类型表是给**值语义与引擎内部对象**用的。控件是"有身份的多态对象 + 由 JS 侧包一层便利层"，
  它的"是什么类"由 JS 的原型链回答就够（`instanceof` 在 JS 侧天然成立），C++ 侧只需要回答
  "这是不是我造的控件实例"（opaque 里有没有守卫）。
- 控件族本来就要在 JS 侧写便利层（对象树登记、保活、`setParent/layout` 语义、属性袋）——
  见 `tree.ts` / `props.ts` / `signals.ts`。既然 JS 侧一定有类层，不如把**类形状**（继承关系、
  构造转发）也交给它，C++ 侧不再重复描述一遍。
- 类描述里的继承字段在 JS 侧是冗余的：`extends` 已经表达得很好，
  而 C++ 侧的 `superCTypeid` 只服务 `canConvert` 判定 —— 透明路径根本不读它（见 §10）。

---

## 2. 三条路

| | A. 引擎注册类 | B. 透明类 | C. 纯 C 函数 |
| --- | --- | --- | --- |
| 类定义在 | C++（`ClassBind` 描述） | **JS**（`classes.ts`） | **JS** |
| 引擎知道这个类吗 | 知道（`m_typeMap`/ctypeid/形参表） | 不知道 | 不知道 |
| 继承怎么表达 | `ClassBind(Child, ParentBind)` → `superCTypeid` | JS `extends` | JS `extends` |
| 实例槽 | 引擎按 `guardedOpaque` 建 opaque | 所有控件共用一个 op class，opaque = 守卫 | 同 B |
| 方法挂在哪 | 引擎建的原型（magic 分发） | **JS 类原型**（magic trampoline） | JS 类原型（裸 C 函数） |
| 构造入口 | `new Cls(...)` → `define.create` | `init_c_<蛇形>(this, ...)` → `define.create` | 同 B |
| `fromJSValue<T>` / `instanceof`(C++ 侧) | 认 | **不认**（只有 op 槽这一条判据） | 不认 |
| 类型契约在哪 | C++ 形参表 + TS d.ts | TS d.ts（形参表只保个数） | TS d.ts |
| 现状用在哪 | 值类型、事件、布局、画笔、`sqlite`/`net`/`worker`/串口/modbus 的类 | **控件族 26 个类（当前实现）** | 少数方法（`def` 的裸函数形式） |

**B 与 C 的差别只是"C++ 侧还要不要一个类描述结构"**：B 用 `jsbind::ClassBind<C>` 收集方法与构造，
C 直接把函数挂上去。功能上等价 —— 因为 `mountTransparentClass` 只读 `ClassBind` 的一小部分字段（§10）。

**当前采用 B，且决定长期保留 `ClassBind`**：它在这里唯一的职责是"把 `def(...)` / `setCreate(...)`
收集成一组函数交给 `mountTransparentClass`"，顺带复用 `def` 的实参转换（`CFunction`）与现成的
`js_JZxxx_ctor`。它**不表示**类被注册进引擎，也**不表达继承**。所以读到控件侧的

```cpp
jsbind::ClassBind<JZWindow> bind_win("JZWindow", bind_w);
```

请只读成"给 `JZWindow` 收集方法"，不要读成"登记一个类 / 声明继承"。§11/§12 的形态 C 是等价备选，暂不做。

---

## 3. 判据：新类走哪条路

1. **是控件（`JZWidget` 及其派生）吗？** → 透明路径（B/C），类写在 `classes.ts`。
2. **是值类型**（`JZPoint`/`JZSize`/`JZRect`/`JZColor`/`JZFont`/`JZImage`/`JZMatrix2D`）吗？ → 注册路径（A）。
   值语义 + 需要当形参/返回值自动转换（`toJsValue` / `fromJSValue`）⇒ 必须进引擎类型表。
3. **是引擎内部对象**（事件、布局、`JZPainter`/`JZPen`/`JZBrush`、`JZSettings`、`jzApp`、`uiLoader`）吗？ → 注册路径（A）。
   它们要做形参校验、要被 `fromJSValue` 取回、要在 C++ 侧判等/`instanceof`。
4. **是别的 JS 模块的类**（`sqlite` / `net` / `worker` / 串口 / modbus）吗？ → 注册路径（A）。
   那族类的 JS 侧只是薄包装，没有"自带类层 + 便利层"的需求。

现状清单（对着代码数）：

- 透明路径：`mountTransparentClass` 26 处调用，覆盖 `classes.ts` 里 `JZWidget` + 25 个派生类
  （`JZJSWidgetModule.cpp:1646` 起、`:2570` 止，含 `:1791` 的 `bind_w` 自己）。
- 注册路径：`registGeometry` / `registColor` / `registImage` / `registEvents` / `registSettings` /
  `registApp` / `registLayouts` / `registPens` / `registMatrix` 里的类，
  以及其它 JS 模块的 `m_engine->registClass(...)`。

---

## 4. 现状 B 的三方分工

### 4.1 JS 侧定义什么（`js_wrapper/widget/classes.ts`）

文件头把设计写死了：

```1:20:JZJSEngine/js_wrapper/widget/classes.ts
// @jz/widget —— 控件族的**类层**(由引擎在装配 widget 模块时加载, 定义全局类)。
//
// 为什么类定义在这里, 而不是 C++ 侧 registClass:
//   控件族对 JZJSEngine **完全透明** —— 引擎不注册它们、没有 ctypeid/类型表, `checkType` /
//   `isInstanceOf` / 形参表都不认控件对象。类型契约由 types/*.d.ts 负责(TS 编译期),
//   运行期只有 op 槽(守卫)与各方法的兜底校验。
//
// C++ 只提供三样东西:
//   ① `__JZWidgetObject` —— 基构造器: 造出"带 op 槽"的实例(quickjs 的 JS_SetOpaque 只对自定义
//      class 生效, 所以实例必须由它造, `class X {}` 直接 new 出来的普通对象挂不上 opaque);
//   ② `init_c_xxx(this, ...args)` —— 造底层 C++ 对象并 set_op(this, 对象);
//   ③ 各原型方法(`c_xxx_yyy`, 原生 quickjs 函数)由 C++ 挂到下面这些原型上。
```

JS 侧实际承担三件事：

1. **类形状**：`JZWidget extends __JZWidgetObject`（基类唯一会造底层对象），
   派生类由 `defWidgetClass(name, parent, initName)` 统一生成 —— `super(NOCREATE)` 跳过祖先、
   再 `init_c_xxx(this, ...args)` 造自己那份，参数**原样转发**给 C++ 的构造形态分派：
   见 `classes.ts:60-87`、`:90-111`、`:113-137`。
2. **幂等安装**：模块可能被求值两次（`classes.js` 与包入口 `index.js` 两条 specifier），
   第二次必须复用同一份类（换原型会丢 C++ 挂上去的方法）：`classes.ts:139-147`。
3. **便利层**（独立文件，都在 `index.ts` 里装配）：
   `props.ts`（`setProperty/property`，JS 侧 WeakMap）、`tree.ts`（`setParent/parent/children/layout` +
   对象树登记）、`signals.ts`（信号属性默认值）、`menus.ts`。

### 4.2 C++ 侧提供什么（三件套）

| 提供物 | 实现位置 | 作用 |
| --- | --- | --- |
| `__JZWidgetObject` 基构造器 | `initWidgetOpClass()`（`JZJSWidgetModule.cpp:279-300`） | 造出带 op 槽的实例：`JS_NewObjectProtoClass(proto, m_opClassId)` + `JS_SetOpaque(obj, nullptr)` |
| `init_c_<蛇形类名>(this, ...)` | `mountTransparentClass()` 内注册（`:514-527`） | 调 `define.create`（就是 `js_JZxxx_ctor`）造 C++ 对象，再 `setOp(this, ptr)` + `attachJsValue` |
| 原型方法 | `mountTransparentClass()`（`:491-512`） | 每个 `CFunction` 挂成 `JS_CFUNC_generic_magic` + `jzMethodTrampoline`；裸函数原样 `JS_NewCFunction` |

方法调用与构造调用的落地：`callTransparentMethod`（`:533-564`）、`callTransparentInit`（`:566-599`）。

### 4.3 装载顺序（不能换）

```1109:1114:JZJSEngine/JSWidget/JZJSWidgetModule.cpp
    // 透明控件类: ① 建 op 槽所需的基构造器/自定义 class; ② 加载 TS 类层(定义全局控件类)。
    // **顺序很讲究**(分三段):
    //   1) 引擎注册的值类型/事件/布局/画笔...(控件族以外的东西) —— TS 类层里的 tree.ts 会往
    //      JZLayout 等**引擎注册类**的原型上装方法, 所以那些类必须先存在;
    //   2) 加载 TS 类层(定义全局控件类 + 装对象树方法);
    //   3) registWidgets/registDialogs 把 C++ 方法挂到这些 TS 类的原型上。
```

即： `initWidgetOpClass` → `registGeometry/.../registMatrix` → `loadWidgetClasses()` →
`registWidgets/registDialogs/registViews()` → `loadWidgetShell()`。

---

## 5. `new JZLabel("x", parent)` 全链路

1. JS：`JZLabel` 的构造器（`defWidgetClass` 生成）→ `super(NOCREATE)` 跳过祖先 → `init_c_label(this, "x", parent)`。
2. 引擎：`init_c_label` 是 `JS_CFUNC_generic_magic`，落到 `jzInitTrampoline` →
   `JZJSWidgetModule::callTransparentInit`（`:566`）。
3. C++：`entry.create(...)` = `js_JZLabel_ctor(...)`（**复用注册路径的构造器**）→ `new JZLabel("x", parent)`。
4. `setOp(jsThis, ptr)`（`:302`）：把 opaque 写成 `new JZObjectPointer(object->guardPointer())` —— 一个**守卫副本**。
5. `attachJsValue(object, jsThis)`：把 JS 对象记到 C++ 对象的动态属性 `js_value` 上（借用）。
6. 装事件过滤器（`:598-599` 起），之后事件/信号都靠 `js_value` 回到这个 JS 对象读回调。

⚠ 第 4 步要求实例能 `JS_SetOpaque` —— 这**只对自定义 class 的实例成立**，
所以 JS 类必须 `extends __JZWidgetObject`，不能用 `class X {}` 造普通对象。

---

## 6. op 槽与守卫：为什么 opaque 不是裸指针

透明路径**自己实现**了注册路径的 `guardedOpaque` 约定，但换了个载体：

- 所有控件实例共用**一个** custom class（`m_opClassId`，`class_name = "JZWidgetObject"`），
  类的身份完全由 JS 原型链表达；C++ 侧只回答"opaque 里有没有守卫"。
- opaque = `JZObjectPointer*`（`JZCore/JZObjectPointer.h:17`，内部 `shared_ptr<JZObjectPointerData>`）。
  它是**共享守卫的堆副本**：`~JZObject` 会把 `data->object` 置空，于是所有副本同时认得出"对象已经没了"。
- 终结器 `jzWidgetOpFinalizer`（`:266-277`）只做一件事：守卫还指着一个活对象就 `delete`，然后删守卫本身。
  ⇒ "底层对象被 C++ 先删掉"（父控件 delete 子控件）与"JS 对象被 GC"两条路都安全，
  不会 UAF、也不会 double free。
- 取 this：`opFromThis`（`:331`）—— 空槽 / 守卫失效 / 非控件对象都**已抛 JS 异常**并返回 `nullptr`。

---

## 7. 宿主句柄 `js_value` 与反向引用

引擎**没有**"底层指针 → JS 对象"的身份表（控件族不进 `m_typeMap`），
反向引用靠"JS 对象诞生时把自己挂到 C++ 对象的动态属性上"：

```2032:2033:JZJSEngine/JZJSEngine.cpp
    if (m_objectCreated && !JS_IsException(obj))
        m_objectCreated(type, obj, ptr);
```

→ `JZJSWidgetModule::onJsObjectCreated`（`:720`）→ `attachJsValue`（`:691`）写 `js_value`。

用途有三：① 事件/信号派发时回 JS 现读属性（`eventFilter`、`callJsCallback`）；
② 信号 connect 时按 JS 类名 + `JS_IsInstanceOf` 认类（`connectSignals`）；
③ 容器 getter 回吐"同一份 JS 对象"：`widgetJsView`（`:700-709`）⇒ `===` 成立。

⚠ 语义：`js_value` 是**借用**（不 dup / 不 free），所有权始终在 JS 侧。C++ 侧不得长期持有。

---

## 8. 引擎侧仅有的两个特判

透明并不彻底，引擎里有两处"专为控件族打的洞"，改绑定时要一起考虑：

1. **守卫识别**：`isGuardedClass` 先认 `m_widgetOpClassId`，否则 `fromJSValue<JZWidget*>` 会把守卫当裸指针交出去。

```2057:2063:JZJSEngine/JZJSEngine.cpp
bool JZJSEngine::isGuardedClass(int class_id) const
{
    // 透明控件类的 op class 也走守卫(opaque = JZObjectPointer*): 它的类没进 m_typeMap(控件族对
    // 引擎透明), 所以由 widget 模块把 class id 告诉引擎 —— 不然 fromJSValue<JZWidget*> 会把守卫
    // 当裸指针交出去。
    if (m_widgetOpClassId != 0 && class_id == m_widgetOpClassId)
        return true;
```

2. **控件对象形参 = `JZObjectPointer*`**（引擎类型号 `JSType_objectPointer`，见 `initTypeMap`）：
   JS 侧的对象形参一律声明成 `JZObjectPointer*`，引擎由此能在**形参层**先校验一次
   "是不是一个活着的对象指针"（`checkFunctionArg` 里判 opaque + 守卫；控件族在类型表里没有
   各自的类型号，所以这一层不走类型号比较）。方法体内再 `dynamic_cast` 到声明的类
   （`widgetFromThis<T>`）—— 传错控件类是 `TypeError`，不是野指针。
   标量/字符串形参仍不做类型校验（类型契约在 `types/*.d.ts`）。

---

## 9. ⚠ 约束与代价（写新代码前先看）

1. **实例必须由 `__JZWidgetObject` 派生**（quickjs 的 `JS_SetOpaque` 只对自定义 class 生效）。
2. **引擎不认控件类型**：`js_check_class_arg<JZWidget>` / `isInstanceOf` / 形参表都不认；
   `instanceof` 只在 JS 侧成立。
3. **形参个数必须精确匹配**：`callTransparentMethod` 里 `argc != need` 直接抛
   （`func->params.size() - 1`，`:541-545`）；类型不校验 ⇒ 传错类型在 C++ 侧可能静默取值。
4. **对象形参一律写 `JZObjectPointer*`**，不要写具体控件类（`JZWidget*` / `JZWindow*` / …）：
   那些类没注册，引擎查不出类型号 ⇒ 形参层查不出来、还会按声明类型 static_cast ⇒ 传错类就是
   类型不符的野指针（UB）。取具体对象用 `widgetFromThis<T>(guard, "方法名")`（内部 `dynamic_cast`
   + 明确报错）。**只有注册过的类**（`JZMenu`/`JZAction`/`JZLayout`/`JZPainter`/`JZImage`/值类型）
   可以保持具体类型 —— 判据是"引擎类型表认不认得这个类型"。
5. **方法里拿 this 一律走 `opFromThis`**（它负责守卫失效报错），不要 `JS_GetOpaque` 硬解。
6. **`js_value`／事件回调属性都是借用语义**；C++ 侧想留就自己 dup。
7. **加载顺序不能换**（§4.3）：TS 类层必须早于挂方法，`tree.ts` 必须晚于它依赖的注册类。
8. **非 `JZObject` 的数据项拿不到宿主句柄**：`JZListWidgetItem` / `JZTableWidgetItem` 不继承 `JZObject`
   （`JZListWidget.h:21`、`JZTableWidget.h:22`），没有动态属性 ⇒ 挂不了 `js_value`，
   所以 item 类接口只能做成"索引式值接口"（`itemData(i)` 一个 `JZVariant` 槽）。
   要给 item 句柄，先给它一个能承载守卫的载体（继承 `JZObject`，或引入专用借用槽）。

---

## 10. `ClassBind` 在透明路径下的字段生效表

`jsbind::ClassBind<C>`（`JZJSBind.h:300-437`）是给**注册路径**设计的描述结构。
透明路径只调 `mountTransparentClass(bind.classDefine())`，而后者只读四个字段：

| `CClassDefine` 字段 | 透明路径是否读 | 说明 |
| --- | --- | --- |
| `name` | ✅ | 找 JS 全局类 + 生成 `init_c_<蛇形>` |
| `create` | ✅ | 注册成 `init_c_<蛇形>`，构造时造 C++ 对象 |
| `functions` | ✅ | 挂到 JS 类原型（magic trampoline） |
| `rawFunctions` | ✅ | 原样 `JS_NewCFunction` |
| `guardedOpaque` | ❌ | 守卫/终结由 `setOp` / `jzWidgetOpFinalizer` 自己实现（全控件共用一个 op class） |
| `destory` | ❌ | 终结器自己 delete；引擎只在注册类的 free 路径上用它（`JZJSEngine.cpp:2222`） |
| `superCTypeid` | ❌ | 继承由 JS `extends` 表达 ⇒ **`ClassBind<JZWindow>("JZWindow", bind_w)` 里的 `bind_w` 是死信息** |
| `pointerSemantics` | ❌ | 引擎身份表不认控件 |
| `propertys` | ❌ | `mountTransparentClass` **不挂属性访问器** ⇒ 控件绑定里写 `defProperty` 是静默无效 |
| `dataFunctions` | ❌ | 同上（透明路径不挂 `func_data` 形式的函数） |

**结论**：`ClassBind` 在控件侧的角色 = **函数收集器**，仅此而已 ——
它实际被读到的只有四个字段（`name` / `create` / `functions` / `rawFunctions`）；
"类"的那部分（继承、opaque 约定、身份表、属性）在控件路径上全是死信息。

**这是刻意保留，不是遗留物**：收集函数总得有个容器，`ClassBind` 现成，
且 `def(...)` 自带实参转换（`CFunction`）、`setCreate` 直接复用 `js_JZxxx_ctor` ——
所以**保留它、不改造**。代价是读代码时要记住"控件侧的 `ClassBind` 不表示注册、不表示继承"，
为此约定三条：

1. ❌ 别指望 `ClassBind<Child>("Child", parentBind)` 的父类参数起作用 —— 继承只认 JS 的 `extends`；
2. ❌ 别在控件绑定里用 `defProperty(...)` / `defData(...)`（透明路径不挂它们，静默无效）；
3. ✅ 新控件照 §13 的写法继续用 `ClassBind` 收集即可 —— 想去掉这层结构只是审美，不是需求（§11）。

---

## 11. 备选形态 C：C++ 只提供纯 C 函数（**暂不采用**）

若哪天觉得"收集器"这层结构多余，可以把它去掉：绑定接口收成三件事（**签名草案**）：

```cpp
// 类名 = JS 全局类名; 函数 = 纯函数。绑定层不认识继承, 也不建任何表。
void mountInit(const char* className, decltype(CClassDefine::create) create);
void mountMethod(const char* className, const char* name, CFunction* fn);   // 保留实参转换(形参表仍只保个数)
void mountRawMethod(const char* className, const char* name, JSCFunction* fn, int length);  // 裸 C 函数
```

对应 C++ 侧的写法（每个类一组）：

```cpp
// 造对象
void* js_JZLabel_ctor(JZJSEngine* engine, int argc, JSValueConst* argv);   // 已有, 复用
// 方法(两种形态, 任选)
JSValue c_label_setText(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv)
{
    JZLabel* w = (JZLabel*)module->opFromThis(ctx, thisVal, "setText");
    if (!w) return JS_EXCEPTION;
    ...
}
```

顺带一提：同一个收集器里其实**两种函数形态可以混用** ——
`bind_w.def("setParentWidget", js_JZWidget_setParentWidget, 1)`（`JZJSBind.h:388-408`）
走的就是裸 C 函数形态；这也是保留 `ClassBind` 的便利之一（要 `this_val` 的方法不必另找入口）。

**收益**：① 一处声明 = 一处事实，不再有"写了但不生效"的字段（§10）；
② `superCTypeid` 这类误导性信息消失，`bind_win("JZWindow", bind_w)` 这种"看起来在建继承"的写法不再出现；
③ 每个控件类在 C++ 侧只剩"构造 + 方法清单"。

**不变的部分**：op 槽 / 守卫 / `js_value` / 事件过滤器 / 信号按 JS 类名连接 —— 这些与"是否用 ClassBind"无关。

### 11.1 为什么不改（2026-09-23 决定）

- **功能零差异**：`mountTransparentClass` 本来只读那四个字段，换入口不改变任何运行期行为；
- **收益只是审美**："少一层结构 / 少几个死字段"——而误用风险已由 §10 的三条约定覆盖；
- **代价不小**：26 个类的调用点全改一遍，还要新造三个挂载接口并重新回归；
- ⇒ **不做**。`ClassBind` 作为函数收集器保留；哪天真要动手，按 §12 走，且当成纯清理提交。

---

## 12. 若将来要做形态 C：迁移步骤（**现在不做**）

> 本节只是预案 —— 结论见 §11.1：`ClassBind` 保留为函数收集器，不改造。

1. **换入口**：把 `mountTransparentClass(const CClassDefine&)` 拆成 §11 的三个函数
   （实现照搬现在 `:477-531` 的函数体，逐条挂；`m_inits` / `m_methods` / `m_rawMethods` 保活逻辑不变）。
2. **改调用点**：`registWidgets()` / `registDialogs()` / `registViews()` 里 26 个类，
   每个类从
   `ClassBind<T> bind_x("T", parentBind); bind_x.def(...); ... ; mountTransparentClass(bind_x.classDefine());`
   改成 `mountInit("T", js_T_ctor); mountMethod("T", ...); mountRawMethod(...);`。
3. **保留 `CFunction`**：`callTransparentMethod` 依赖它的形参表与 `call()`（实参转换），
   这部分与 `ClassBind` 无关，继续用。
4. **顺手核对**：控件侧若有 `defProperty` / `defData` 的写法，迁移时删掉（本来就不生效，§10）。
5. **回归**：`ctest -R jsengine` + `test_jsui`（控件族的主覆盖），
   重点是"同一底层对象 ⇒ 同一 JS 对象"（`widgetJsView` 的 `===`）与信号/事件派发。

> 注意：这是**纯清理**（去掉误导性的死字段），不是行为改动 ——
> 现在也没有把控件注册进引擎，所以不需要动引擎的类型表。

---

## 13. 新增一个控件类的清单（当前做法：形态 B + `ClassBind` 收集）

1. `JZWidget/JZFoo.h/.cpp`：C++ 控件本体（`JZObject` 派生）。
2. `JZJSEngine/JSWidget/JZJSWidget.h/.cpp`：`js_JZFoo_ctor`（形态分派到 C++ 构造重载）。
3. `JZJSEngine/JSWidget/JZJSWidgetModule.cpp`：
   - `registWidgets()` 里 `ClassBind<JZFoo> bind_foo("JZFoo", bind_w);` + 若干 `def(...)`（或裸函数）；
   - `defSignalProperty(bind_foo, "onXxx", &JZFoo::sigXxx)`；
   - 末尾 `mountTransparentClass(bind_foo.classDefine());`。
4. `js_wrapper/widget/classes.ts`：`defWidgetClass('JZFoo', JZWidget, 'init_c_foo')` + 加进
   `__jzWidgetClasses` 与导出列表。
5. `js_wrapper/widget/types/`：新类写 `types/foo.d.ts`（或在 `views.d.ts`/`widgets.d.ts` 里加），
   并在 `index.d.ts` / `ambient.d.ts` 的清单里挂上。
6. 常量走 `defClassInt("JZFoo", "SomeEnum", value)`（挂构造器，不是原型）。
7. 编译 `tsc -b js_wrapper`（产出 `dist/`），跑 `test_jsui` 加一两条用例。
