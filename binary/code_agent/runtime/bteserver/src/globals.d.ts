// ---------------------------------------------------------------------------
// bteserver 里"不在 TS 源码中声明"的全局名字 —— 分两类, **性质完全不同**:
//
// A) 还没迁完的 Qt 名字 —— 2026-09-14 之前由 src/qtcompat.ts 那个 Qt 运行期替身提供;
//    **该文件已删除**, 所以这些名字运行期没有任何实现(碰到的调用点会 undefined 报错),
//    这里保留宽松声明只是为了: ① 让还没收口的界面层文件能通过 tsc;
//    ② 把"还剩哪些 Qt 名字要迁"这件事留在类型层可见。
//    声明一律用 `declare const X: any`(不是 declare class): 业务里既 `new X()` 又
//    `X.静态成员` 两种用法都有, any 才能同时罩住(类的静态侧没法加索引签名)。
//    → 迁完一个删一条; 全部删光 = 界面层迁移完成。
//
//    2026-09-18 收口进度: 列表/表格的 item 对象(JZTableWidgetItem 等)、单元格控件
//    (QCheckBox/setCellWidget)、输入校验器(QIntValidator/QDoubleValidator/QValidator)、
//    弹窗与布局类(QDialog/QVBoxLayout/QHBoxLayout/QGridLayout/QLabel/QLineEdit/QPushButton)、
//    Qt 常量(Qt.LeftButton 之类)全部迁完(见各文件的"原 Qt ..."注释)。
//    **只剩两处**:
//      * `QMenu`(3 个右键菜单, BTEServer.ts) —— 引擎侧值级菜单接口已就绪:
//        `new JZMenu()` → `addAction(text)` 返回下标 → `onActionTriggered = (i) => {...}` →
//        `popup(win, new JZRect(globalX, globalY, 1, 1))`; `menu.exec(pos)` 那种**同步**取选中项
//        的写法引擎没有(弹层是异步的, 靠回调)。改完把这条声明删掉。
//      * `QTreeWidgetItem`(StepDataDialog.ts 的工步树) —— 引擎的 JZTreeWidget 在 C++ 侧是
//        **指针式 item**, JS 侧还没有对应接口(要有 value 级的树接口才能迁)。
//
// B) 引擎的真实全局(见 JZJSEngine/js_wrapper 里部署的 jz_types) ——
//    JZ* 控件/画笔/几何类型、jzApp/jzWidget/uiLoader、各 netPack 的 stru_* 包类、
//    sqlite3_* / workbook_* 等原生库函数。这些是**真东西**, 大多数已经在
//    js_wrapper 的 .d.ts 里(user 工程把它拷成 jz_types/ 后 include 即可);
//    这里只补本工程还引用的少数几个(或与 jz_types 重复但被 skipLibCheck 放过的)。
// ---------------------------------------------------------------------------

// ---- A) 待迁移的 Qt 名字(运行期已无实现; 迁完即删) ----
declare const QMenu: any;
declare const QTreeWidgetItem: any;
// 第三方 Qt 库(随工程分发, 不参与 JS 移植; 调用点按 Qt API 原样保留)
declare const QCustomPlot: any;
declare const QCP: any;
declare const QCPAxisTickerTime: any;

// ---- B) 引擎的真实全局 ----
// JZWidget 家族/值类型(JZPoint/JZRect/JZColor/JZPen/JZFont/JZPainter/...)、
// JZMessageBox/JZFileDialog、jzWidget/uiLoader 的类型都在 js_wrapper 的 .d.ts 里
// (tsconfig 的 include 直接指向仓库的 js_wrapper), 所以这里**不重复声明**。
// 下面这几个是 js_wrapper 还没写 .d.ts 的(声明按 JZJSWidgetModule 的实际绑定写):
//
// JZSettings —— registSettings(): 打开一个 ini, 读/写键值。
//   注意绑定只收"不需要默认值参数"的形式: getValue(name) 没读到返回空串, getInt(name) 返回 0,
//   要默认值就在 JS 侧写 `s.getValue('x') || 'def'`。
declare class JZSettings {
  constructor();
  /** 打开(不存在则新建) ini 文件 */
  open(file: string): boolean;
  sync(): void;
  beginGroup(group: string): void;
  endGroup(): void;
  hasGroup(group: string): boolean;
  hasValue(name: string): boolean;
  setValue(name: string, value: string): void;
  getValue(name: string): string;
  setBool(name: string, value: boolean): void;
  getBool(name: string): boolean;
  setInt(name: string, value: number): void;
  getInt(name: string): number;
  setDouble(name: string, value: number): void;
  getDouble(name: string): number;
}

// JZDesktopServices —— registApp(): 打开链接/文件(等价 Qt QDesktopServices::openUrl)。
declare const JZDesktopServices: {
  openUrl(url: string): boolean;
};

// jzApp —— registApp(): 应用级入口(C++ 的 JZApplication 在 JS 侧的门面)。
//   applicationDirPath() 入口脚本所在目录(没加载过脚本时是空串);
//   workingDirPath() 进程当前工作目录; quit()/exit(code) 请求事件循环退出。
declare const jzApp: {
  applicationDirPath(): string;
  applicationFilePath(): string;
  workingDirPath(): string;
  quit(): void;
  exit(code?: number): void;
};

// 其余用到的原生/库函数与报文包类(引擎注入, 见结构体/协议层的注册)
declare const PlaySoundW: any;
declare const RaiseException: any;
declare const SQLITE_DONE: any;
declare const SQLITE_OK: any;
declare const SQLITE_ROW: any;
declare const __jz: any;
declare const format_set_bg_color: any;
declare const format_set_num_format: any;
declare const qInstallMessageHandler: any;
// 注: TRUE / FALSE / NULL 这类 Win32/Qt 常量**故意不声明** —— 它们原来由 qtcompat 导出,
//     各文件已就地改成 false/true/null; 留着宽松声明只会把真正的缺名字错误藏起来。
// sqlite3_* / libxlsxwriter / zip 的 C API(引擎注入的 WASM/原生库)
declare const sqlite3_bind_blob: any;
declare const sqlite3_bind_int: any;
declare const sqlite3_close: any;
declare const sqlite3_column_blob: any;
declare const sqlite3_column_bytes: any;
declare const sqlite3_column_int: any;
declare const sqlite3_exec: any;
declare const sqlite3_open: any;
declare const sqlite3_prepare: any;
declare const sqlite3_prepare_v2: any;
declare const sqlite3_reset: any;
declare const sqlite3_step: any;
declare const unzip_mem: any;
declare const zip_mem: any;
declare const workbook_add_format: any;
declare const workbook_add_worksheet: any;
declare const workbook_close: any;
declare const workbook_new: any;
declare const worksheet_write_number: any;
declare const worksheet_write_string: any;
// stru_* 报文包类: 由协议层在运行期挂到全局(structs.js / netPackDefine.js)
declare const stru_BTS_CMD_RESUME: any;
declare const stru_BTS_CMD_RESUME_RESP: any;
declare const stru_BTS_CMD_SEND_PARALLEL_CANCEL: any;
declare const stru_BTS_CMD_START70: any;
declare const stru_BTS_CMD_STEP_JUMP: any;
declare const stru_BTS_CMD_STEP_JUMP_RESP: any;
declare const stru_BTS_CMD_STOP: any;
declare const stru_BTS_CMD_STOP_RESP: any;
