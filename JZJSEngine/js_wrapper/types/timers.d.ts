// 定时器 / 引擎助手 —— JZJSEngine::init 直接装到全局对象上的函数(见 JZJSEngine.cpp
// JZJSEngineInternalFunction)。lib.es2025 里没有它们(DOM/Node 才定义 setInterval),
// 所以在这里补声明, 与运行期保持一致。

/**
 * 阻塞当前线程 interval 毫秒(正整数; 否则抛 TypeError)。
 * 引擎线程上长阻塞会卡住事件泵, 一般只在脚本/测试里用。
 */
declare function sleep(interval: number): void;

/**
 * 周期任务: 每 interval 毫秒调用一次 callback(interval 必须是**正整数**, 0 抛 TypeError)。
 * 返回任务 id(给 clearInterval 用)。
 */
declare function setInterval(callback: () => void, interval: number): number;

/**
 * 一次性任务: interval 毫秒后调用 callback(允许 0 = 下一轮事件泵尽快执行)。
 * 返回任务 id(给 clearTimeout 用)。
 */
declare function setTimeout(callback: () => void, interval: number): number;

/** 取消 setInterval 的周期任务 */
declare function clearInterval(id: number): void;

/** 取消 setTimeout 的一次性任务 */
declare function clearTimeout(id: number): void;

/** 引擎助手(全局对象 engine) */
declare const engine: {
  /** 当前是否处于"请求中断"状态(脚本被要求停止时用于自行退出长循环) */
  isInterruption(): boolean;
};
