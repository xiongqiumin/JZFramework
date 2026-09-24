// 定时器 / 引擎助手 —— JZJSEngine::init 直接装到全局对象上的函数(见 JZJSEngine.cpp
// JZJSEngineInternalFunction)。lib.es2025 里没有它们(DOM/Node 才定义 setInterval),
// 所以在这里补声明, 与运行期保持一致。

/**
 * 阻塞当前线程 interval 毫秒(正整数; 否则抛 TypeError)。
 * 引擎线程上长阻塞会卡住事件泵, 一般只在脚本/测试里用。
 */
declare function sleep(interval: number): void;

/**
 * setTimeout / setInterval 的返回值 —— 对齐 Node 的 Timeout 对象(不是数字):
 * 带 ref()/unref()/hasRef()/refresh()。
 *
 * ⭐ ref 语义(与 Node 一致): **默认 ref** —— 还挂着的定时器会让宿主留在事件循环里;
 *   `unref()` 之后它不再算"有事要等"(进程可能就此退出), 但**任务本身照常到期触发** ——
 *   只要进程因别的原因还活着。`ref()` 恢复默认。
 * 兼容: 对象带 valueOf() = 任务 id, 所以把返回值当数字用(+t / 比较 / clearTimeout(+t))仍然有效;
 *   clearTimeout/clearInterval 两种入参(Timeout 对象或数字 id)都收。
 */
declare interface Timeout {
  /** 任务 id(内部用; 也是 valueOf() 的返回值) */
  readonly _id: number;
  /** 恢复"算一票让宿主活着"(默认状态) */
  ref(): this;
  /** 不再吊住宿主(任务照常触发, 见上) */
  unref(): this;
  /** 当前是否"算一票"(任务已被 clear 掉返回 false) */
  hasRef(): boolean;
  /** 从"现在"重新计时; 不给间隔就沿用原来的(Node 同名方法) */
  refresh(ms?: number): this;
  valueOf(): number;
}

/**
 * 周期任务: 每 interval 毫秒调用一次 callback(interval 必须是**正整数**, 0 抛 TypeError)。
 * 返回 Timeout(unref() 可让它不吊住宿主)。
 */
declare function setInterval(callback: () => void, interval: number): Timeout;

/**
 * 一次性任务: interval 毫秒后调用 callback(允许 0 = 下一轮事件泵尽快执行)。
 * 返回 Timeout(unref() 可让它不吊住宿主)。
 */
declare function setTimeout(callback: () => void, interval: number): Timeout;

/** 取消 setInterval 的周期任务(Timeout 对象或数字 id 都收) */
declare function clearInterval(id: number | Timeout): void;

/** 取消 setTimeout 的一次性任务(Timeout 对象或数字 id 都收) */
declare function clearTimeout(id: number | Timeout): void;

/** 引擎助手(全局对象 engine) */
declare const engine: {
  /** 当前是否处于"请求中断"状态(脚本被要求停止时用于自行退出长循环) */
  isInterruption(): boolean;
};
