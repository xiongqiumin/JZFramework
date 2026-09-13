// 全局 uiLoader 命名空间(JZJSWidgetModule::registUiLoader)。
//
// 两个工厂: 解析 Qt 风格的 UI 定义并**直接构造真实控件树**, 返回根 JZWindow 窗口。
// 根窗口上还挂着按 id 命名子控件(运行期动态附加, 故只能声明成索引签名)以及内部保活数组
// __keep(无 id 的子树由它保住, 否则会被 JS GC 回收而销毁底层控件)。
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
