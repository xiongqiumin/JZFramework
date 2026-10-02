#ifndef JZ_EMULATORVIEWJS_H_
#define JZ_EMULATORVIEWJS_H_

class JZJSEngine;

// ============================================================================
// 把 JZEmulatorView 挂到 JS 类层的原型上（"透明控件类"路径，与 JZPushButton 同一机制）
// ============================================================================
// 为什么注册代码在本模块，而不是在 JZJSEngine/JSWidget/JZJSWidgetModule.cpp 里：
//   JZEmulator 是**单例模块**（独立静态库，依赖方向 JZEmulator → JZFramework 单向）。
//   而 JZJSWidgetModule.cpp 属于 JZFramework —— 在那里 new JZEmulatorView 就是反向依赖，
//   链接根本不成立（何况 JZEmulatorView 还持有 JZEmulatorCore，绕不开）。
//   所以：类定义 + 方法挂载都留在本模块，由宿主在 widget 模块 initEngine() 之后显式调一次。
//
// 类定义在这里 eval（**不改 js_wrapper**）：widget/classes.ts 是对外的公共类层，
//   本控件属于"装了模块才有"的内部能力，不该混进公共包。副作用是好的：零改动 .ts
//   ⇒ 不用 tsc、不用 package.py，也就绕开了"改了 .ts 忘了重编译 dist"那个坑。
//
// ⚠ 调用时机（重要）：必须在 JZJSWidgetModule::initEngine() **之后** ——
//   eval 那段要拿全局 __JZWidgetObject / JZWidget，挂方法也要求类已经存在，
//   否则 mountTransparentClass 只打一行日志然后**静默跳过**（控件"没有方法"却看不出原因）。
//   宿主调用点见 binary/code_agent/JZEngineRuntime.cpp（用 JZ_HAS_EMULATOR 宏守卫）。
//
// 引擎里没装本模块时，JS 侧 `JZEmulatorView` 类仍然存在（在 classes.ts 里），
// 但 new 它会在构造器里抛"引擎没有注册 init_c_emulator_view"—— 报错清晰，不会静默坏掉。
void jzRegisterEmulatorView(JZJSEngine* engine);

#endif // JZ_EMULATORVIEWJS_H_
