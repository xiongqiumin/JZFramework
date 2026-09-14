// 模态对话框 —— 全局类/对象(JZJSWidgetModule::registDialogs)。
//
// ============================ 模态语义 ============================
// exec() 会**重入事件循环**(嵌套栈帧, 见 JZWin32Thread::execModal): 期间照常派发事件、跑 JS 回调
// 与定时器, 所以 exec() 返回后"世界可能已经变了" —— 与 Qt 相同的约定。
// 同一个对话框对象嵌套 exec() 会被拒绝: 打警告并返回 -1(对齐 QDialog::exec 的
// "Recursive call detected"); 不同对话框互相嵌套开模态是允许的。
//
// ============================ 生命周期 ============================
// JZMessageBox.* / JZFileDialog.* 是**一次性**接口(内部 exec, 结束后延后删除), 不把对话框句柄交给 JS。
// 需要自定义内容/按钮时用 `new JZDialog()` 自己搭(它是 JZObject, 走普通控件那套所有权规则)。
//
// ============================ 删除时机 ============================
// 回调里删控件请用 `widget.deleteLater()`: 模态循环/事件派发期间直接销毁, 会让外层正在派发的事件
// 对象悬垂; deleteLater 排到本轮事件处理完之后(等价 Qt 的 deleteLater)。

/** @final */
declare class JZDialog extends JZWindow {
  /** 无父; 想要"模态禁用 owner"+ 相对 owner 居中, 先 setParentWidget(owner) 再 exec() */
  constructor();
  /**
   * 模态执行: 阻塞到 accept() / reject() / 窗口关闭为止。
   * @returns 1 = Accepted, 0 = Rejected, -1 = 递归调用被拒(同 Qt, 见文件头说明)
   */
  exec(): number;
  /** 结束模态并把结果置为 Accepted(1); 发 onAccepted / onFinished */
  accept(): void;
  /** 结束模态并把结果置为 Rejected(0); 发 onRejected / onFinished */
  reject(): void;
  /** 上一次 exec() 的结果(1 / 0; 还没跑过时是 0) */
  result(): number;
  /** 属性形: 赋值设 / 覆盖 / `= null` 清空 */
  onAccepted: (() => void) | null;
  onRejected: (() => void) | null;
  /** 回调带 (result: number) */
  onFinished: ((result: number) => void) | null;
}

/** 模态消息框(静态接口; 结果为 1 = 确认 / 0 = 关闭或取消) */
declare const JZMessageBox: {
  information(parent: JZWidget, title: string, text: string): number;
  warning(parent: JZWidget, title: string, text: string): number;
  /** OK / Cancel 两个按钮: 1 = OK, 0 = Cancel */
  question(parent: JZWidget, title: string, text: string): number;
};

/**
 * 原生文件对话框(模态; 期间照常跑 JS 回调与定时器)。
 * filter 形如 "Images (*.png *.jpg);;Text (*.txt)", 空串 = All Files; 取消返回 null / 空数组。
 */
declare const JZFileDialog: {
  getOpenFileName(parent?: JZWidget | null, caption?: string, dir?: string, filter?: string): string | null;
  getOpenFileNames(parent?: JZWidget | null, caption?: string, dir?: string, filter?: string): string[];
  getSaveFileName(parent?: JZWidget | null, caption?: string, dir?: string, filter?: string): string | null;
  getExistingDirectory(parent?: JZWidget | null, caption?: string, dir?: string): string | null;
};
