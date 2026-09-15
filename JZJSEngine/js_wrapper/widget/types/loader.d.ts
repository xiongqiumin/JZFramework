// 应用级全局对象: uiLoader(JZJSWidgetModule::registUiLoader 注册) 与 jzApp(registApp 注册)。

// ---------------------------------------------------------------------------
// uiLoader: 解析 Qt 风格 UI 定义并**直接构造真实控件树**, 返回根 JZWindow 窗口
// ---------------------------------------------------------------------------
// 根窗口上还挂着按 id 命名的子控件(运行期动态附加, 故只能声明成索引签名)。
//
// 已知限制:
//   * 控件类型必须在 uiClassMap(JZJSUiLoader.cpp)里且**已注册到引擎**: 其中 'Canvas' -> 'JZCanvas'
//     这一项指向的类并不存在(窗口/画布那套代码已从模块里移除), 用到 Canvas 节点的 UI 会构造失败;
//   * 解析/构造失败时抛异常(不是返回空对象), 失败原因在 C++ 侧 loader.error() 里;
//   * 只应用几何/文本/标题/可见性/样式表这几类属性(setText/setTitle/setGeometry/setVisible/
//     setStyleSheet), 其它属性被忽略(见 JZJSUiLoader 的 UiScriptGen)。

/** load / loadFile 的返回: 根窗口 + 按 id 挂上的子控件 */
type JZUiRoot = JZWindow & { [id: string]: any };

declare const uiLoader: {
  /** 从 UI 文件加载并构造控件树(文件不存在/解析失败抛异常) */
  loadFile(file: string): JZUiRoot;
  /** 从 UI 文本(XML)加载并构造控件树 */
  load(text: string): JZUiRoot;
};

// ---------------------------------------------------------------------------
// jzApp: 进程级应用对象(JZApplication)的 JS 入口。路径/退出/全局样式表都在这里。
// (单控件级样式用 widget.setStyleSheet(css))
// ---------------------------------------------------------------------------
declare const jzApp: {
  /** 入口脚本所在目录(没加载过脚本时返回空串) */
  applicationDirPath(): string;
  /** 入口脚本绝对路径(没加载过脚本时返回空串) */
  applicationFilePath(): string;
  /** 进程当前工作目录 */
  workingDirPath(): string;
  /** 请求事件循环退出(等价 JZApplication::quit) */
  quit(): void;
  /** 以退出码退出(等价 JZApplication::exit) */
  exit(code?: number): void;
  /**
   * 设置**全局**样式表(等价 C++ 的 JZApplication::setStyleSheet), 作用于所有控件。
   * 换肤 = 整表替换: 变量只有 :root 一处, 且是 parse 期文本替换, 所以切主题要把两份 CSS
   * 各完整给一次, 而不是"改一个变量"。语法错误抛 SyntaxError(错误串见 styleSheetError())。
   */
  setStyleSheet(css: string): boolean;
};
