// JZEvent 及其子类 —— 框架派发给 JS 的事件对象(全局类; registEvents)。
//
// 用法: 不自己 new。事件由框架创建并派发, 在控件的 onXxx 属性回调里**接收**:
//   win.onMousePress = (e) => { const x = e.localX(); const y = e.localY(); };
//
// 有效期: 事件对象只在**本次派发期间**有效 —— 回调返回后绑定层立刻让它失效, 存到外面再用会抛
// "指针已被释放"(与 C++ 侧"事件是栈对象"一致)。需要留数据就把值取出来存。
//
// 继承: 全部是 JZEvent 派生(`e instanceof JZEvent` 恒为 true; C++ 侧 JZResizeEvent 也已改为
// JZEvent 派生 —— 尺寸事件同样经 sendEvent 进对象, 才过得了事件过滤器链);
// enter/leave/focus 回调收到的是按 **JZEvent** 包装的对象(派发点形参即 JZEvent*),
// 拿不到 JZEnterEvent/JZLeaveEvent 这类 C++ 子类型(它们没有注册)。

/** @final */
declare class JZEvent {
  /** 事件由框架创建并派发, 不要自己 new */
  protected constructor();

  /**
   * 事件类型号(见 JZCore/JZEvent.h 的 JZEvent_xxx):
   * 0=none 1=deleteObject 2=timer 3=asyncSignal 4=netReq 5=netRes 6=log 7=co
   * 8=invalidate 9=paint 10=show 11=hide 12=resize 13=key 14=mouse 15=enter 16=leave 17=close
   */
  type(): number;
  accept(): void;
  ignore(): void;
  /** 默认 true; 处理不了的键盘/IME 事件应 ignore() 让它继续上传 */
  isAccepted(): boolean;
}

/** @final */
declare class JZMouseEvent extends JZEvent {
  protected constructor();

  /** 相对目标控件左上角的逻辑坐标(命中分发时回填) */
  localX(): number;
  localY(): number;
  /** 屏幕逻辑坐标 */
  globalX(): number;
  globalY(): number;
  /** localX() 的别名 */
  x(): number;
  /** localY() 的别名 */
  y(): number;
  /** 0=NoButton 1=Left 2=Right 3=Middle(没有挂成常量, 直接比数字) */
  button(): number;
  /** 滚轮格数(120 的原始增量 / 120) */
  wheelDelta(): number;
  /** 修饰键位组合: 1=Shift 2=Ctrl 4=Alt */
  modifiers(): number;
}

/** @final */
declare class JZKeyEvent extends JZEvent {
  protected constructor();

  /** 框架统一键码(与 Win32 VK_* 同值, 见 JZCore 侧 JZKey) */
  key(): number;
  /** 修饰键位组合: 1=Shift 2=Ctrl 4=Alt */
  modifiers(): number;
  /** 该按键产生的可打印文本(UTF-8); 功能键为空。IME 上屏内容不走这里 */
  text(): string;
  /** 长按产生的重复键 */
  isRepeat(): boolean;
  /** 按下 / 释放 */
  isPress(): boolean;
  shift(): boolean;
  ctrl(): boolean;
  alt(): boolean;
}

// 尺寸变化事件。C++ 侧 JZResizeEvent 是 JZEvent 派生(注册时挂到 JZEvent 原型下),
// 因此它同样有 type()/accept()/ignore()/isAccepted(); 另外多出尺寸/旧尺寸查询。
/** @final */
declare class JZResizeEvent extends JZEvent {
  /** 事件由框架创建并派发, 不要自己 new */
  protected constructor();

  width(): number;
  height(): number;
  oldWidth(): number;
  oldHeight(): number;
  size(): JZSize;
  oldSize(): JZSize;
}

/** @final */
declare class JZPainterEvent extends JZEvent {
  protected constructor();

  /** 本次重绘区域(脏区 ∩ 控件 rect, 控件本地坐标) */
  rect(): JZRect;
}

// 显示 / 隐藏 / 关闭事件: 无自有成员, 只有 JZEvent 的 type()/accept()/ignore()/isAccepted()。
// 注: JZCloseEvent 虽然注册了, 但 JS 侧**没有**对应的回调 —— 事件过滤器不转发 closeEvent
// (它是"窗口"的概念, 见 JZWindow::closeEvent), 窗口关闭无法从 JS 否决。
/** @final */
declare class JZShowEvent extends JZEvent {
  protected constructor();
}
/** @final */
declare class JZHideEvent extends JZEvent {
  protected constructor();
}
/** @final */
declare class JZCloseEvent extends JZEvent {
  protected constructor();
}

// 定时器事件(C++ 直接派发时用; JS 侧定时任务用 setInterval/setTimeout)
/** @final */
declare class JZTimerEvent extends JZEvent {
  protected constructor();
  id(): number;
}

// 未暴露: JZInvalidateEvent / JZEnterEvent / JZLeaveEvent(只按 JZEvent 包装给 JS)。
