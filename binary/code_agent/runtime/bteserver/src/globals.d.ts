// bteserver 迁移用的全局声明 —— 这些名字**不在 TS 源码里**, 由运行期提供:
//   * Qt 兼容层(qtcompat.ts 装到全局的 Q*/Ui_*Class/枚举), 见 qtcompat 的 qtCompatReport;
//   * 原生/WASM 库(sqlite3_* / worksheet_* / workbook_* / zip_mem 等)与 Win32(qApp/SND_*/PlaySoundW)。
// 声明刻意宽松(索引签名 + any): 这些 API 没有可用的类型信息, 目标是"类型检查能过、行为不变";
// 将来若把 qtcompat 也迁成强类型, 可以逐条收紧(或换成真实的 .d.ts)。

// ---- Qt 兼容层 / UI 生成类: 当类用(可 new / 可 extends, 实例成员任意) ----
declare class Nightcharts {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QAbstractItemView {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QActionGroup {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QCP {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QCPAxisTickerTime {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QCheckBox {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QCustomPlot {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QDesktopServices {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QDialog {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QDoubleValidator {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QGridLayout {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QHBoxLayout {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QHeaderView {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QIntValidator {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QKeySequence {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QLabel {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QLineEdit {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QListWidgetItem {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QMainWindow {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QMenu {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QPainter {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QPushButton {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QShortcut {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QTableWidget {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QTableWidgetItem {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QTreeWidgetItem {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QVBoxLayout {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QValidator {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class QWidget {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class Ui_BTEServerClass {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class Ui_DeviceMacDialogClass {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class Ui_HelpDialogClass {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class Ui_SplitDialogClass {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class Ui_SplitSettingWidgetClass {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class Ui_StepDataDialogClass {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class Ui_StepEditDialogClass {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class Ui_StepInfoDialogClass {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class Ui_StepJumpDialogClass {
  [key: string]: any;
  constructor(...args: any[]);
}
declare class Ui_StepSettingDialogClass {
  [key: string]: any;
  constructor(...args: any[]);
}

// ---- 值 / 函数型全局(qApp、库函数、Windows 常量、引擎注入的 __jz) ----
declare const PlaySoundW: any;
declare const RaiseException: any;
declare const SQLITE_DONE: any;
declare const SQLITE_OK: any;
declare const SQLITE_ROW: any;
declare const __jz: any;
declare const format_set_bg_color: any;
declare const format_set_num_format: any;
declare const qApp: any;
declare const qInstallMessageHandler: any;
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
declare const stru_BTS_CMD_RESUME: any;
declare const stru_BTS_CMD_RESUME_RESP: any;
declare const stru_BTS_CMD_SEND_PARALLEL_CANCEL: any;
declare const stru_BTS_CMD_START70: any;
declare const stru_BTS_CMD_STEP_JUMP: any;
declare const stru_BTS_CMD_STEP_JUMP_RESP: any;
declare const stru_BTS_CMD_STOP: any;
declare const stru_BTS_CMD_STOP_RESP: any;
declare const unzip_mem: any;
declare const workbook_add_format: any;
declare const workbook_add_worksheet: any;
declare const workbook_close: any;
declare const workbook_new: any;
declare const worksheet_write_number: any;
declare const worksheet_write_string: any;
declare const zip_mem: any;
