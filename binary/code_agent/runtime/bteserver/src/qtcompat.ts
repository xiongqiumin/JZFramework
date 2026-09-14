// ---------------------------------------------------------------------------
// qtcompat.js —— Qt 运行期替身(把 Qt 语义"搬到 JS 引擎里", 而不是改掉各业务文件)
//
// 为什么需要它
// ------------
// bteserver 的 JS 是 C++/Qt 源码的**逐行翻译**: 3000+ 处调用 Qt 类, 但 JS 引擎里
// 没有 QApplication/QTableWidget/QDataStream/... 本文件在**不改业务文件**的前提下:
//
//   1) "真实现"——非 GUI 且语义能一一对应的类: QFile/QDir/QTextStream/QSettings/
//      QObject/QColor/QPoint/QRect/... 这些是配置读取、文件操作真正需要的能力,
//      按 Qt 语义实打实实现。
//      **已删除并换回 JS 原生**:
//        QTimer                 -> setInterval/setTimeout
//        QDate/QTime/QDateTime  -> Date + ./datetime.js
//        QByteArray/QDataStream -> 存档改走 JSON(./storage.js): 存档格式不要求与 C++
//                                  兼容, 直接 JSON.stringify/parse, 不再自造二进制流
//                                  (网络报文的字节布局仍由 protocol.js/structs.js 保证一致)
//
//   2) "空壳"——GUI 类(QTableWidget/QGridLayout/QMessageBox/QPainter/Ui_*...):
//      用 Proxy 生成"任何方法都能调、任何属性都能取"的宽容对象, 使**非 GUI 代码**
//      (网络/协议/数据模型)可以在无界面环境下被加载与执行, 同时把"被调用到的、
//      但尚未实现的能力"记账到 QtCompat.report() —— 这份账单就是 GUI 移植的待办清单。
//
//   3) "桥"——Qt 里由 Qt 自己提供的全局设施(文件路径、播放声音、退出):
//      QApplication.applicationDirPath() -> __jz.core;
//      PlaySoundW / RaiseException -> 无害替身(见文件末)。
//
// 用法(每个依赖 Qt 全局名字的模块, 在**第一条 import** 处加载本文件):
//     import './qtcompat.js';
//   ES 模块的 import 先于本模块体求值, 所以模块顶层的 `new QColor(...)` 也能拿到替身。
//
// 注意: 本文件刻意**不 import 任何东西**(不依赖 core/dist、不依赖 __jz 一定存在),
//       只做 globalThis 注入; 缺 __jz.core 时文件/配置类退化为内存实现并记账。
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 0. 记账器: 记录"被调到但还没真实现"的 Qt 能力
// ---------------------------------------------------------------------------
const g_missing = new Map();   // 名字 -> 次数

// 诊断: 本模块被求值了几次(>1 说明引擎把"绝对路径 import"和"相对路径 import"
// 当成两个模块了 —— 那么 qtCompat.report() 读到的就不是业务代码用的那一份)。
try { globalThis.__qtcompatLoads = (globalThis.__qtcompatLoads || 0) + 1; } catch (e) { }

function noteMissing(name)
{
    g_missing.set(name, (g_missing.get(name) || 0) + 1);
}

// ---------------------------------------------------------------------------
// 1. 引擎适配: 文件系统 / 应用目录(有 __jz.core 就用真的, 否则退化)
// ---------------------------------------------------------------------------
function jzCore()
{
    try
    {
        // typeof 对未声明标识符是安全的
        if (typeof __jz !== "undefined" && __jz && __jz.core)
            return __jz.core;
    }
    catch (e) { /* ignore */ }
    return null;
}

function fsExists(path)
{
    const c = jzCore();
    return c ? !!c.fileExists(path) : false;
}
function fsReadText(path)
{
    const c = jzCore();
    if (!c)
        return "";
    const u8 = c.readFile(path);
    if (!u8)
        return "";
    if (typeof __jz.bytes !== "undefined" && __jz.bytes)
        return __jz.bytes.toString(u8);
    let s = "";
    for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
    return s;
}
// 原始字节读(文本用 fsReadText; QFile 的内部缓冲需要字节)
function fsReadBytes(path)
{
    const c = jzCore();
    if (!c)
        return new Uint8Array(0);
    const u8 = c.readFile(path);
    return u8 ? new Uint8Array(u8) : new Uint8Array(0);
}
function fsWrite(path, u8, append)
{
    const c = jzCore();
    if (!c)
    {
        noteMissing("QFile::write(无 __jz.core, 未落盘)");
        return false;
    }
    return !!c.writeFile(path, u8, !!append);
}
function fsMkdir(path)
{
    const c = jzCore();
    return c ? !!c.dirMkdir(path, true) : false;
}
function fsList(path)
{
    const c = jzCore();
    if (!c)
        return [];
    const list = c.dirEntryList(path);
    return list ? list.map((e) => e.name) : [];
}
function fsRemove(path)
{
    const c = jzCore();
    return c ? !!c.fileRemove(path) : false;
}
function dirName(path)
{
    const c = jzCore();
    if (c)
        return c.dirName(path);
    const i = Math.max(String(path).lastIndexOf("/"), String(path).lastIndexOf("\\"));
    return i > 0 ? String(path).slice(0, i) : "";
}

// 引擎的应用级入口(全局 jzApp, 见 JZJSWidgetModule::registApp —— 包着 C++ 的 JZApplication):
// 路径与 quit/exit 的事实来源。老引擎没有这个全局时返回 null, 调用方退回 __jz.core / 记账。
function jzAppObj()
{
    try
    {
        // 走 globalThis 取(与 jzCore 一样对未声明标识符容错), 老引擎没有该全局时返回 null
        const g = (typeof globalThis !== "undefined") ? (globalThis as any) : null;
        const a = g ? g.jzApp : null;
        if (a)
            return a;
    }
    catch (e) { }
    return null;
}

// QApplication::applicationDirPath() / applicationFilePath() 的取值:
//   Qt 里它们指"可执行文件"; 但本运行时是"一个 app 可以起多个引擎, 每个引擎有自己的**入口脚本**"
//   (一般是 main.js), 移植过来的界面代码用它们定位**随脚本走**的资源(option.ini / Modules /
//   lang / wav / Data), 所以默认取入口脚本那一份:
//     applicationFilePath() = 入口脚本绝对路径
//     applicationDirPath()  = 入口脚本所在目录
//   优先问 jzApp(引擎侧统一算好); 注意 jzApp 对**没加载过脚本**的引擎返回**空串**(它不臆造 exe 那份),
//   这时兼容层自己继续往下退: __jz.core.appFile -> workDir(运行根) -> 可执行文件目录(旧行为)。
//   setApplicationDirPath() 可随时显式覆盖。
let g_appDir = null;
function appFilePath()
{
    const a = jzAppObj();
    if (a && a.applicationFilePath)
    {
        const f = a.applicationFilePath();
        if (f)
            return String(f);
    }
    const c = jzCore();
    if (!c)
        return "";
    if (c.appFile)
    {
        const f = c.appFile();
        if (f)
            return f;
    }
    return c.execPath();
}
function appDirPath()
{
    if (g_appDir !== null)
        return g_appDir;
    const a = jzAppObj();
    if (a && a.applicationDirPath)
    {
        const d = a.applicationDirPath();
        if (d)
            return String(d);
    }
    const c = jzCore();
    if (!c)
        return ".";
    if (c.appFile)
    {
        const f = c.appFile();
        if (f)
            return dirName(f);
    }
    if (c.workDir)
    {
        const wd = c.workDir();
        if (wd)
            return wd;
    }
    return dirName(c.execPath());
}

// ---------------------------------------------------------------------------
// 2. 小工具
// ---------------------------------------------------------------------------
function toU8(data)
{
    if (data === null || data === undefined)
        return new Uint8Array(0);
    if (data instanceof Uint8Array)
        return data;
    if (data instanceof ArrayBuffer)
        return new Uint8Array(data);
    return new Uint8Array(0);
}
function concatU8(a, b)
{
    const out = new Uint8Array(a.length + b.length);
    out.set(a, 0);
    out.set(b, a.length);
    return out;
}
function u8Equals(a, b)
{
    if (a.length !== b.length)
        return false;
    for (let i = 0; i < a.length; i++)
        if (a[i] !== b[i]) return false;
    return true;
}
function utf8Of(s)
{
    const c = jzCore();
    if (c && typeof __jz !== "undefined" && __jz.bytes)
        return __jz.bytes.fromString(String(s));
    const out = [];
    for (let i = 0; i < String(s).length; i++)
    {
        const code = String(s).charCodeAt(i);
        if (code < 0x80) out.push(code);
        else if (code < 0x800) out.push(0xc0 | (code >> 6), 0x80 | (code & 0x3f));
        else out.push(0xe0 | (code >> 12), 0x80 | ((code >> 6) & 0x3f), 0x80 | (code & 0x3f));
    }
    return new Uint8Array(out);
}
function textOfUtf8(u8)
{
    const c = jzCore();
    if (c && typeof __jz !== "undefined" && __jz.bytes)
        return __jz.bytes.toString(u8);
    let s = "";
    for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
    return s;
}
function pad2(v) { return String(v).padStart(2, "0"); }
function pad3(v) { return String(v).padStart(3, "0"); }

// ---------------------------------------------------------------------------
// 3. Qt 枚举 / 全局常量
// ---------------------------------------------------------------------------
// 具名颜色(Qt.white / Qt.black / Qt.darkBlue ...)是**运行期回填**的(见下面的 Qt.white = null
// 与 QColor 定义后的回填循环), 所以这里显式标成"任意键"—— 否则 TS 按对象字面量推断, 回填的名字全都报错。
export const Qt: { [k: string]: any } = {
    // 鼠标
    NoButton: 0, LeftButton: 1, RightButton: 2, MidButton: 4, MiddleButton: 4,
    // 右键策略
    NoContextMenu: 0, DefaultContextMenu: 1, CustomContextMenu: 2, ActionsContextMenu: 3,
    // 角色
    DisplayRole: 0, DecorationRole: 1, EditRole: 2, ToolTipRole: 3, StatusTipRole: 4,
    CheckStateRole: 10, UserRole: 0x0100, UserProperty: 0x0101,
    // 对齐
    AlignLeft: 0x0001, AlignRight: 0x0002, AlignHCenter: 0x0004, AlignJustify: 0x0008,
    AlignTop: 0x0020, AlignBottom: 0x0040, AlignVCenter: 0x0080,
    AlignCenter: 0x0084, AlignAbsolute: 0x0010,
    // item flags
    NoItemFlags: 0, ItemIsSelectable: 1, ItemIsEditable: 2, ItemIsDragEnabled: 4,
    ItemIsDropEnabled: 8, ItemIsUserCheckable: 16, ItemIsEnabled: 32, ItemIsTristate: 64,
    // 键盘修饰
    NoModifier: 0x00000000, ShiftModifier: 0x02000000, ControlModifier: 0x04000000,
    AltModifier: 0x08000000, MetaModifier: 0x10000000, KeypadModifier: 0x20000000,
    // 窗口标志
    Widget: 0x00000000, Window: 0x00000001, Dialog: 0x00000002,
    WindowCloseButtonHint: 0x08000000, WindowMinimizeButtonHint: 0x04000000,
    WindowMaximizeButtonHint: 0x02000000, WindowMinMaxButtonsHint: 0x06000000,
    WindowContextHelpButtonHint: 0x00010000, WindowSystemMenuHint: 0x00002000,
    WindowTitleHint: 0x00001000, CustomizeWindowHint: 0x02000000,
    // 方向
    Horizontal: 1, Vertical: 2,
    // 文本交互
    TextSelectableByMouse: 1, TextEditable: 2,
    // 其余
    CaseSensitive: 0, CaseInsensitive: 1,
    KeepAspectRatio: 1, SmoothTransformation: 2,
    ElideRight: 2, ElideMiddle: 3,
    // 消息框返回值
    Yes: 0x00004000, No: 0x00010000, Ok: 0x00000400, Cancel: 0x00400000,
    // 笔/画刷样式
    NoPen: 0, SolidLine: 1, DashLine: 2, DotLine: 3,
    NoBrush: 0, SolidPattern: 1, CrossPattern: 9,
    // 关键键
    Key_Escape: 0x01000000, Key_Return: 0x01000004, Key_Enter: 0x01000005,
    Key_Space: 0x20, Key_Delete: 0x01000007, Key_F1: 0x01000030,
};

// Qt 的具名颜色(QColor), 业务代码里 Qt.red / Qt.darkBlue 之类的入参
Qt.white = null;   // 由下方 QColor 定义后回填
Qt.black = null;

// QtMsgType
export const QtDebugMsg = 0, QtInfoMsg = 4, QtWarningMsg = 1, QtCriticalMsg = 2, QtFatalMsg = 3;
const QtMsgType = { QtDebugMsg, QtInfoMsg, QtWarningMsg, QtCriticalMsg, QtFatalMsg };

// Win32 常量(AlarmPlayer / main.js 里按原样保留的调用)
export const NULL = null;
export const TRUE = 1, FALSE = 0;
export const SND_SYNC = 0x0000, SND_ASYNC = 0x0001, SND_NODEFAULT = 0x0002,
    SND_MEMORY = 0x0004, SND_LOOP = 0x0008, SND_FILENAME = 0x00020000;
export const EXCEPTION_ACCESS_VIOLATION = 0xc0000005;

// ---------------------------------------------------------------------------
// 4. 值类型
// ---------------------------------------------------------------------------
export class QColor
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor(r, g, b, a)
    {
        if (r instanceof QColor)
        {
            this._r = r._r; this._g = r._g; this._b = r._b; this._a = r._a;
        }
        else if (r === undefined)
        {
            this._r = 0; this._g = 0; this._b = 0; this._a = 255;
        }
        else if (typeof r === "string")
        {
            const c = parseColorName(r);
            this._r = c[0]; this._g = c[1]; this._b = c[2]; this._a = 255;
        }
        else
        {
            this._r = r | 0; this._g = g | 0; this._b = b | 0;
            this._a = (a === undefined) ? 255 : (a | 0);
        }
    }
    red() { return this._r; }
    green() { return this._g; }
    blue() { return this._b; }
    alpha() { return this._a; }
    setRed(v) { this._r = v | 0; }
    setGreen(v) { this._g = v | 0; }
    setBlue(v) { this._b = v | 0; }
    setAlpha(v) { this._a = v | 0; }
    equals(o) { return o instanceof QColor && o._r === this._r && o._g === this._g && o._b === this._b && o._a === this._a; }
    isValid() { return true; }
    name() { return "#" + [this._r, this._g, this._b].map((v) => v.toString(16).padStart(2, "0")).join(""); }
    rgb() { return (0xff000000 | (this._r << 16) | (this._g << 8) | this._b) >>> 0; }
    toString() { return this.name(); }
}
function parseColorName(name)
{
    const T = {
        white: [255, 255, 255], black: [0, 0, 0], red: [255, 0, 0], darkRed: [139, 0, 0],
        green: [0, 255, 0], darkGreen: [0, 100, 0], blue: [0, 0, 255], darkBlue: [0, 0, 139],
        cyan: [0, 255, 255], darkCyan: [0, 139, 139], magenta: [255, 0, 255], darkMagenta: [139, 0, 139],
        yellow: [255, 255, 0], darkYellow: [128, 128, 0], gray: [128, 128, 128], darkGray: [169, 169, 169],
        lightGray: [211, 211, 211], orange: [255, 165, 0], transparent: [0, 0, 0],
    };
    return T[name] || [0, 0, 0];
}
export function qRgb(r, g, b) { return (0xff000000 | (r << 16) | (g << 8) | b) >>> 0; }
function qRgba(r, g, b, a) { return ((a << 24) | (r << 16) | (g << 8) | b) >>> 0; }

export class QPoint
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor(x, y) { this._x = x | 0; this._y = y | 0; }
    x() { return this._x; }
    y() { return this._y; }
    setX(v) { this._x = v | 0; }
    setY(v) { this._y = v | 0; }
    rx() { return this._x; }
    ry() { return this._y; }
    equals(o) { return o instanceof QPoint && o._x === this._x && o._y === this._y; }
    toString() { return this._x + "," + this._y; }
}

class QSize
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor(w, h) { this._w = w | 0; this._h = h | 0; }
    width() { return this._w; }
    height() { return this._h; }
    setWidth(v) { this._w = v | 0; }
    setHeight(v) { this._h = v | 0; }
    isEmpty() { return this._w <= 0 || this._h <= 0; }
}

export class QRect
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor(a, b, c, d)
    {
        if (a instanceof QPoint && b instanceof QPoint)
        {
            this._x1 = Math.min(a.x(), b.x()); this._y1 = Math.min(a.y(), b.y());
            this._x2 = Math.max(a.x(), b.x()); this._y2 = Math.max(a.y(), b.y());
        }
        else
        {
            this._x1 = (a | 0); this._y1 = (b | 0);
            this._x2 = this._x1 + (c | 0) - 1; this._y2 = this._y1 + (d | 0) - 1;
        }
    }
    left() { return this._x1; }
    top() { return this._y1; }
    right() { return this._x2; }
    bottom() { return this._y2; }
    width() { return this._x2 - this._x1 + 1; }
    height() { return this._y2 - this._y1 + 1; }
    x() { return this._x1; }
    y() { return this._y1; }
    topLeft() { return new QPoint(this._x1, this._y1); }
    bottomRight() { return new QPoint(this._x2, this._y2); }
    center() { return new QPoint(((this._x1 + this._x2) / 2) | 0, ((this._y1 + this._y2) / 2) | 0); }
    isEmpty() { return this._x2 < this._x1 || this._y2 < this._y1; }
    isValid() { return !this.isEmpty(); }
    contains(p) { return p.x() >= this._x1 && p.x() <= this._x2 && p.y() >= this._y1 && p.y() <= this._y2; }
    adjusted(dx1, dy1, dx2, dy2)
    {
        const r = new QRect(0, 0, 0, 0);
        r._x1 = this._x1 + dx1; r._y1 = this._y1 + dy1;
        r._x2 = this._x2 + dx2; r._y2 = this._y2 + dy2;
        return r;
    }
}

export class QPen
{
    constructor(a, b)
    {
        if (a instanceof QColor || typeof a === "number")
        {
            this._color = (a instanceof QColor) ? a : new QColor(a);
            this._width = (b === undefined) ? 1 : b;
            this._style = 1;
        }
        else
        {
            this._color = new QColor(0, 0, 0);
            this._width = 1;
            this._style = a || 1;
        }
    }
    color() { return this._color; }
    setColor(c) { this._color = (c instanceof QColor) ? c : new QColor(c); }
    width() { return this._width; }
    setWidth(w) { this._width = w; }
    style() { return this._style; }
    setStyle(s) { this._style = s; }
}

export class QBrush
{
    constructor(c) { this._color = (c instanceof QColor || typeof c === "number") ? new QColor(c) : new QColor(0, 0, 0); }
    color() { return this._color; }
    setColor(c) { this._color = (c instanceof QColor) ? c : new QColor(c); }
    style() { return 1; }
}

class QFont
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor(family, size)
    {
        this._family = family || "sans-serif";
        this._size = size || 12;
        this._pixelSize = 0;
        this._bold = false;
    }
    family() { return this._family; }
    setFamily(f) { this._family = f; }
    pointSize() { return this._size; }
    setPointSize(s) { this._size = s; }
    pixelSize() { return this._pixelSize || this._size; }
    setPixelSize(s) { this._pixelSize = s; }
    setBold(b) { this._bold = !!b; }
    bold() { return this._bold; }
    setPointSizeF(s) { this._size = s; }
}

export class QFontMetrics
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor(font) { this._font = font; }
    horizontalAdvance(t) { return String(t).length * ((this._font && this._font.pixelSize()) || 12); }
    width(t) { return this.horizontalAdvance(t); }
    height() { return ((this._font && this._font.pixelSize()) || 12) + 2; }
    boundingRect(t) { return new QRect(0, 0, this.horizontalAdvance(t), this.height()); }
}

export class QLinearGradient
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor() { this._stops = []; this._c0 = new QPoint(0, 0); this._c1 = new QPoint(0, 1); }
    setStart(x, y) { this._c0 = new QPoint(x, y); }
    setFinalStop(x, y) { this._c1 = new QPoint(x, y); }
    setColorAt(pos, color) { this._stops.push({ pos, color }); }
}

export class QTextOption
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor(align) { this._align = align || 0; }
    setAlignment(a) { this._align = a; }
    alignment() { return this._align; }
}

// ---------------------------------------------------------------------------
// 5. 时间 / 日期
// ---------------------------------------------------------------------------
// 注: QDate / QTime / QDateTime **整节已删除**(2026-09-13) —— 时间一律用 JS 原生的 Date,
//   配合 ./datetime.js 的工具函数(它们只补 Qt 有而 JS 没有的那点语义, 不引入包装类型):
//     formatDateTime(date, "yyyy-MM-dd HH:mm:ss")   原 QDateTime::toString / QDate::toString
//     addSeconds(date, n) / addDays(date, n)        原 addSecs / addDays
//     secondsBetween(a, b) / daysBetween(a, b)      原 secsTo / daysTo(任一无效 -> 0, 同 Qt)
//     isBefore(a, b) / isAfter(a, b)                原 lt / gt(任一无效 -> false)
//     asDate(v)                                     把"可能不是 Date 的值"归一成 Date 或 null
//   约定: **"未设置"一律用 null** —— 原 C++ `QDateTime()` 默认构造出来是无效值,
//         而 JS 的 `new Date()` 是**当前时间**, 两者语义相反(最容易写错的一处)。
//   QTime 在 C++ 里只是"从 0 点起的毫秒"容器(DataTrans 的时长换算), JS 侧按毫秒直接算。
//   Date 的序列化见 ./storage.js(存档 JSON: Date <-> ISO 字符串)。

// ---------------------------------------------------------------------------
// 8. 文件 / 目录 / 文本流 / 配置
// ---------------------------------------------------------------------------
const QIODevice = { NotOpen: 0, ReadOnly: 1, WriteOnly: 2, ReadWrite: 3, Append: 4, Truncate: 8, Text: 16, Unbuffered: 32 };

export class QFile
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // 内部缓冲直接用 Uint8Array(原 QByteArray); 文本走 utf8Of/textOfUtf8 转换。
    // 注: 只有**写模式**(WriteOnly/Append)打开时 close() 才落盘 —— 原来读模式的 close()
    //     也会把缓冲写回去(等于重写原文件), 是个隐患, 顺手修掉。
    constructor(name)
    {
        this._name = String(name || "");
        this._open = false;
        this._writable = false;
        this._buf = new Uint8Array(0);
        this._pos = 0;
    }
    // 这里刻意不继承 QIODevice(它只是常量桶), 保持轻量
    fileName() { return this._name; }
    setFileName(n) { this._name = String(n); }
    exists() { return fsExists(this._name); }
    size() { return this._buf.length; }
    atEnd() { return this._pos >= this._buf.length; }
    open(flags)
    {
        flags = flags || 0;
        if (flags & QIODevice.ReadOnly)
        {
            if (!fsExists(this._name))
                return false;
            this._buf = fsReadBytes(this._name);
            this._pos = 0;
            this._open = true;
            this._writable = false;
            return true;
        }
        // WriteOnly / Truncate / Append
        const append = !!(flags & QIODevice.Append) && fsExists(this._name);
        this._buf = append ? fsReadBytes(this._name) : new Uint8Array(0);
        this._pos = this._buf.length;
        this._open = true;
        this._writable = true;
        if (!append)
            fsWrite(this._name, new Uint8Array(0), false);   // Truncate
        return true;
    }
    close()
    {
        if (!this._open)
            return;
        this._open = false;
        if (this._writable)
            fsWrite(this._name, this._buf, false);
    }
    flush() { if (this._open && this._writable) fsWrite(this._name, this._buf, false); }
    // 返回 Uint8Array(文本请自行 bytesToText/toString) —— 原 QByteArray 已删除
    readAll() { const b = this._buf.slice(this._pos); this._pos = this._buf.length; return b; }
    read(n) { const b = this._buf.slice(this._pos, this._pos + n); this._pos += n; return b; }
    readLine() { return textOfUtf8(this.readAll()); }
    write(data)
    {
        const add = (typeof data === "string") ? utf8Of(data) : toU8(data);
        this._buf = concatU8(this._buf, add);
        this._pos = this._buf.length;
        return true;
    }
    remove() { return fsRemove(this._name); }
    static get ReadOnly() { return QIODevice.ReadOnly; }
    static get WriteOnly() { return QIODevice.WriteOnly; }
    static get ReadWrite() { return QIODevice.ReadWrite; }
    static get Append() { return QIODevice.Append; }
    static get Truncate() { return QIODevice.Truncate; }
    static get Text() { return QIODevice.Text; }
    static exists(p) { return fsExists(p); }
    static remove(p) { return fsRemove(p); }
    static copy() { noteMissing("QFile::copy"); return false; }
    static rename(a, b) { noteMissing("QFile::rename"); return false; }
}

export class QDir
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor(path, nameFilter, sort, filters)
    {
        this._path = (path === undefined || path === null) ? "." : String(path);
        this._filters = filters;
        this._nameFilter = nameFilter;
    }
    path() { return this._path; }
    absolutePath() { return this._path; }
    exists() { return fsExists(this._path); }
    mkdir(name) { return fsMkdir(joinPath(this._path, name)); }
    mkpath(name) { return fsMkdir(joinPath(this._path, name)); }
    filePath(name) { return joinPath(this._path, name); }
    entryList() { return fsList(this._path); }
    entryInfoList() { return fsList(this._path).map((n) => ({ fileName: () => n })); }
    removeRecursively() { noteMissing("QDir::removeRecursively"); return false; }
    static currentPath() { const c = jzCore(); return c ? c.cwd() : "."; }
    static separator() { return "\\"; }
    static exists(p) { return fsExists(p); }
    static mkpath(p) { return fsMkdir(p); }
}
function joinPath(a, b)
{
    if (!a) return String(b);
    if (!b) return String(a);
    // b 本身是绝对路径(如 "F:/x" 或 "/x")时直接用 b —— 业务里有 `new QDir().mkdir(绝对路径)`
    // 这种写法(BTEProject::init), 拼接会得到 "./F:/x" 这种废路径。
    if (/^([A-Za-z]:[\\/]|[\\/])/.test(String(b)))
        return String(b);
    const sep = String(a).indexOf("\\") >= 0 ? "\\" : "/";
    return String(a).replace(/[\/\\]+$/, "") + sep + String(b);
}

export class QTextStream
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor(device, mode)
    {
        this._dev = device;
        this._lines = null;
        this._idx = 0;
    }
    // 解析时按行惰性切分
    _ensure()
    {
        if (this._lines !== null)
            return;
        const text = (this._dev instanceof QFile) ? textOfUtf8(this._dev.readAll())
            : String(this._dev || "");
        this._lines = text.length ? text.split(/\r?\n/) : [];
    }
    atEnd() { this._ensure(); return this._idx >= this._lines.length; }
    readLine() { this._ensure(); return this._lines[this._idx++] || ""; }
    readAll() { this._ensure(); const s = this._lines.slice(this._idx).join("\n"); this._idx = this._lines.length; return s; }
    setCodec() { return this; }
    setEncoding() { return this; }
    write(s) { if (this._dev instanceof QFile) this._dev.write(String(s)); return this; }
}

export class QSettings
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor(fileName, format, parent)
    {
        // QSettings(QString, Format[, QObject*]) / QSettings(Format, Scope, org, app)
        this._file = (typeof fileName === "string") ? fileName : null;
        this._map = new Map();
        this._dirty = false;
        if (this._file && fsExists(this._file))
            this._load();
    }
    _load()
    {
        const text = fsReadText(this._file);
        let section = "";
        const lines = String(text).split(/\r?\n/);
        for (const line of lines)
        {
            const s = line.trim();
            if (!s || s.charAt(0) === ";" || s.charAt(0) === "#")
                continue;
            if (s.charAt(0) === "[" && s.charAt(s.length - 1) === "]")
            {
                section = s.slice(1, -1);
                continue;
            }
            const eq = s.indexOf("=");
            if (eq < 0)
                continue;
            const key = s.slice(0, eq).trim();
            const val = s.slice(eq + 1).trim();
            this._map.set(section ? (section + "/" + key) : key, val);
        }
    }
    value(key, def)
    {
        if (this._map.has(key))
            return this._map.get(key);
        // Qt 的 value() 返回 QVariant, 这里返回原始字符串; 业务侧多会 Number()/( == ) 比较
        return (def === undefined) ? "" : def;
    }
    setValue(key, v) { this._map.set(key, String(v)); this._dirty = true; }
    contains(key) { return this._map.has(key); }
    remove(key) { this._map.delete(key); this._dirty = true; }
    allKeys() { return Array.from(this._map.keys()); }
    sync()
    {
        if (!this._dirty || !this._file)
            return;
        const groups = new Map();
        for (const [k, v] of this._map)
        {
            const i = k.lastIndexOf("/");
            const sec = (i > 0) ? k.slice(0, i) : "";
            if (!groups.has(sec)) groups.set(sec, []);
            groups.get(sec).push([(i > 0) ? k.slice(i + 1) : k, v]);
        }
        let out = "";
        if (groups.has(""))
        {
            for (const [k, v] of groups.get("")) out += k + "=" + v + "\n";
            groups.delete("");
        }
        for (const [sec, list] of groups)
        {
            out += "[" + sec + "]\n";
            for (const [k, v] of list) out += k + "=" + v + "\n";
        }
        fsWrite(this._file, utf8Of(out), false);
        this._dirty = false;
    }
    static get IniFormat() { return 1; }
    static get NativeFormat() { return 0; }
    static get UserScope() { return 0; }
    static get SystemScope() { return 1; }
}

// ---------------------------------------------------------------------------
// 9. 对象 / 定时器 / 应用
// ---------------------------------------------------------------------------
class QObject
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor(parent)
    {
        this._parent = parent || null;
        this._objectName = "";
        this._deleted = false;
    }
    parent() { return this._parent; }
    setParent(p) { this._parent = p || null; }
    objectName() { return this._objectName; }
    setObjectName(n) { this._objectName = String(n); }
    deleteLater() { this._deleted = true; }
    sender() { return null; }
    // 事件过滤器等: 无 GUI 时按"不拦截"返回
    installEventFilter() { }
    removeEventFilter() { }
    connect() { noteMissing("QObject::connect(信号槽数组是 JS 侧手写, connect 只记账)"); }
    static connect() { noteMissing("QObject::connect"); }
}

// 注: QTimer 已删除(2026-09-13) —— 定时器一律用 JS 原生的
//   setInterval(() => slot(), ms) / clearInterval(id) / setTimeout(cb, ms)。
//   Qt 里"定时器是对象 + timeout 信号"在本移植里没有价值: 每个定时器只有一个槽。
//   BTEServer 的 m_heartTimer/m_autoTimer/m_stepTimer 现在存的就是 setInterval 句柄。

export class QApplication extends QObject
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor(argc, argv)
    {
        super(null);
        this._argv = argv || [];
        this.sigAboutToQuit = [];
        this.sigLastWindowClosed = [];
        if (!QApplication._inst)
            QApplication._inst = this;
    }
    exec() { noteMissing("QApplication::exec(JS 引擎事件循环由引擎托管, 无需 exec)"); return 0; }
    // 有全局 jzApp 就转发到真应用对象(等于 JZApplication::quit/exit), 否则退回记账
    quit() { const a = jzAppObj(); if (a) a.quit(); else noteMissing("QApplication::quit"); }
    exit(code) { const a = jzAppObj(); if (a) a.exit(code | 0); else noteMissing("QApplication::exit"); }
    processEvents() { }
    translate(context, text) { return text; }
    installTranslator(t) { this._translator = t; }
    removeTranslator(t) { if (this._translator === t) this._translator = null; }
    arguments() { return this._argv; }
    keyboardModifiers() { return Qt.NoModifier; }
    // Qt 的 setStyleSheet 是"整表替换": 框架侧对应全局 jzWidget.setStyleSheet(css)
    // (绑定层提供; 变量/组件规则见 test_jswidget/main.js 的换肤演示)
    setStyleSheet(css)
    {
        const jw = globalThis.jzWidget;
        if (jw && typeof jw.setStyleSheet === "function")
        {
            jw.setStyleSheet(String(css));
            return;
        }
        noteMissing("QApplication::setStyleSheet(引擎没注册 jzWidget.setStyleSheet)");
    }
    // Qt 里下面这些是 static 成员, 但 C++ 允许 `qApp->xxx()` 这样经实例调用,
    // 业务代码(如 BTEServer 构造里的 option.ini 路径)正是这么写的 —— 实例上也得有。
    applicationDirPath() { return QApplication.applicationDirPath(); }
    setApplicationDirPath(p) { QApplication.setApplicationDirPath(p); }
    applicationFilePath() { return QApplication.applicationFilePath(); }
    applicationName() { return this._appName || ""; }
    setApplicationName(n) { this._appName = String(n); }
    applicationVersion() { return this._appVersion || ""; }
    setApplicationVersion(v) { this._appVersion = String(v); }
    organizationName() { return this._orgName || ""; }
    setOrganizationName(n) { this._orgName = String(n); }
    instance() { return QApplication._inst; }
    primaryScreen() { return QApplication.primaryScreen(); }
    closeAllWindows() { if (QApplication.closeAllWindows) QApplication.closeAllWindows(); }
    setOverrideCursor() { noteMissing("QApplication::setOverrideCursor"); }
    restoreOverrideCursor() { }
    setQuitOnLastWindowClosed() { }
    static get ApplicationFlags() { return 0; }
    static applicationDirPath() { return appDirPath(); }
    static setApplicationDirPath(p) { g_appDir = String(p); }
    static applicationFilePath() { return appFilePath(); }
    static quit() { const a = jzAppObj(); if (a) a.quit(); else noteMissing("QApplication::quit"); }
    static exit(code) { const a = jzAppObj(); if (a) a.exit(code | 0); else noteMissing("QApplication::exit"); }
    static translate(context, text) { return text; }
    static instance() { return QApplication._inst; }
    static primaryScreen() { return makeLenient("QScreen"); }
    static setOverrideCursor() { noteMissing("QApplication::setOverrideCursor"); }
    static restoreOverrideCursor() { }
    static closeAllWindows() { }
    // Qt 里这些也是 static, 业务代码常按 `QApplication::xxx()` 调(如 mouseReleaseEvent
    // 里判断 Ctrl 修饰键) —— 实例上有了, 静态入口也得有, 不然 "not a function"。
    static keyboardModifiers() { return Qt.NoModifier; }
    static queryKeyboardModifiers() { return Qt.NoModifier; }
    static mouseButtons() { return Qt.NoButton; }
    static activeWindow() { return null; }
    static activeModalWidget() { return null; }
    static focusWidget() { return null; }
    static topLevelWidgets() { return []; }
    static allWidgets() { return []; }
    static sendEvent() { }
    static postEvent() { }
    static desktop() { return makeLenient("QDesktopWidget"); }
    static clipboard() { return makeLenient("QClipboard"); }
    static get KeypadModifier() { return Qt.KeypadModifier; }
    static get ControlModifier() { return Qt.ControlModifier; }
    static get ShiftModifier() { return Qt.ShiftModifier; }
}
QApplication._inst = null;

export class QTranslator extends QObject
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor(parent) { super(parent); this._loaded = false; }
    load(a, b) { this._loaded = true; noteMissing("QTranslator::load(.qm 未接入, 直接当作加载成功)"); return true; }
    isEmpty() { return !this._loaded; }
}

export class QSharedMemory extends QObject
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor(key, parent) { super(parent); this._key = key; this._attached = false; }
    attach() { return false; }
    detach() { this._attached = false; }
    create(size) { this._attached = true; return true; }
    isAttached() { return this._attached; }
    lock() { return true; }
    unlock() { return true; }
    data() { return null; }
}

// ---------------------------------------------------------------------------
// 线程: QThread -> 框架真 worker(引擎的 JZJSWorkerModule)
//
//   引擎现在有**真 worker**(Node worker_threads 风格): 每个 worker 一个独立 JSRuntime +
//   独立线程 + 全新空 global, 只有一条通信路 = 消息(结构化克隆的字节)。
//     JS 侧: 全局 Worker 类: new Worker("入口脚本"[, {workerData}])
//                              w.postMessage(v) / w.on('exit', cb) / w.terminate()
//                              w.id / w.exited / w.exitCode
//            worker 内(全局): __jz.worker.onMessage((from, value) => ...)
//                            __jz.worker.postTo(id, value) / selfId() / parentId() /
//                            isMainThread() / ids() / childIds() / workerData / exit()
//
//   与 Qt 的差别(所以不能照抄):
//     - Qt 的 QThread 是"子类覆写 run()"的**同进程共享内存**模型; worker 是独立 runtime,
//       **闭包不能跨引擎传**, 所以 run() 搬不到子线程里执行 —— 必须给**入口脚本**。
//     - 主线程只有**一个** `__jz.worker.onMessage` 槽位, 多个线程/业务同时收消息会互相顶掉,
//       所以本文件统一接管该槽位, 再按 from(worker id) 派发(见 installWorkerDispatch)。
//
//   用法(与 Qt 的差异就一条: 用 setEntry 代替覆写 run):
//     const t = new QThread();
//     t.setEntry("worker/recorder.js");            // 相对 workDir, 或绝对路径
//     t.sigMessage.push((from, v) => { ... });     // worker 里 postTo(t.workerId(), v)
//     t.sigFinished.push((code) => { ... });
//     t.start();
//     t.postMessage({ op: "open", path: "..." });
// ---------------------------------------------------------------------------
const g_workerThreads = new Map();   // worker id -> QThread
const g_workerMessageCbs = [];       // 通用回调(不属于某个 QThread 的消息)
let g_workerDispatchInstalled = false;

function workerApi()
{
    const jz = globalThis.__jz;
    return (jz && jz.worker) ? jz.worker : null;
}

// 接管唯一的 onMessage 槽位, 按 from 派发; 幂等
function installWorkerDispatch()
{
    const api = workerApi();
    if (!api || typeof api.onMessage !== "function")
        return false;
    if (g_workerDispatchInstalled)
        return true;

    api.onMessage((from, value) =>
    {
        const t = g_workerThreads.get(from);
        if (t)
        {
            emitSignal(t.sigMessage, from, value);
            emitSignal(t.sigResult, value);
        }
        for (let i = 0; i < g_workerMessageCbs.length; i++)
        {
            try { g_workerMessageCbs[i](from, value); }
            catch (e) { console.error("[qtcompat] worker 消息回调异常: " + e); }
        }
        return value;
    });
    g_workerDispatchInstalled = true;
    return true;
}

// 收"任意 worker"的消息(不属于某个 QThread 的场景)
export function onWorkerMessage(cb)
{
    g_workerMessageCbs.push(cb);
    installWorkerDispatch();
    return cb;
}

class QThread extends QObject
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor(parent)
    {
        super(parent);
        this.sigStarted = [];
        this.sigFinished = [];
        this.sigMessage = [];
        this.sigResult = [];
        this.m_entry = "";
        this.m_workerData = undefined;
        this.m_worker = null;
        this.m_running = false;
        this.m_exitCode = 0;
    }

    // Qt 里是覆写 run(); 这里必须给**入口脚本**(worker 是独立 runtime)
    setEntry(path) { this.m_entry = String(path); return this; }
    entry() { return this.m_entry; }
    setWorkerData(v) { this.m_workerData = v; return this; }

    start()
    {
        installWorkerDispatch();
        const hasWorker = (typeof globalThis.Worker === "function");

        if (!hasWorker || !this.m_entry)
        {
            // 没有 worker 模块 / 没给入口脚本: 不做"假线程" —— 记账, 并在当前线程跑一次 run()
            // (与 Qt 的"start 后 run 会被调用"保持可预期, 但绝不能假装是子线程)
            noteMissing(hasWorker
                ? "QThread::start 需要入口脚本(worker 是独立 runtime, run() 搬不过去): 先 setEntry('xxx.js')"
                : "QThread::start(引擎没有 worker 模块)");
            emitSignal(this.sigStarted);
            if (typeof this.run === "function")
                this.run();
            emitSignal(this.sigFinished, 0);
            return false;
        }

        const w = (this.m_workerData === undefined)
            ? new globalThis.Worker(this.m_entry)
            : new globalThis.Worker(this.m_entry, { workerData: this.m_workerData });
        this.m_worker = w;
        this.m_running = true;
        if (w.id !== undefined && w.id !== null)
            g_workerThreads.set(w.id, this);

        w.on("exit", (code) =>
        {
            this.m_running = false;
            this.m_exitCode = code | 0;
            if (w.id !== undefined && w.id !== null)
                g_workerThreads.delete(w.id);
            emitSignal(this.sigFinished, code | 0);
        });

        emitSignal(this.sigStarted);
        return true;
    }

    workerId() { return this.m_worker ? this.m_worker.id : -1; }
    postMessage(v)
    {
        if (!this.m_worker || typeof this.m_worker.postMessage !== "function")
            return false;
        this.m_worker.postMessage(v);
        return true;
    }
    // 软退出: 约定消息 __qt_quit, 由 worker 脚本自己收尾(框架没有"请求退出"原语)
    quit() { return this.postMessage({ __qt_quit: true }); }
    // 硬终止(框架 terminate)
    terminate()
    {
        if (!this.m_worker || typeof this.m_worker.terminate !== "function")
            return false;
        this.m_worker.terminate();
        return true;
    }
    wait() { noteMissing("QThread::wait(JS 无阻塞等待: 用 sigFinished 或 waitFinishedAsync)"); return !this.m_running; }
    isRunning() { return this.m_running; }
    exitCode() { return this.m_exitCode; }
    isFinished() { return !this.m_running; }

    // await 版等待(JS 侧替代 Qt 的阻塞 wait())
    waitFinishedAsync(timeoutMs)
    {
        const self = this;
        return new Promise((resolve) =>
        {
            if (!self.m_running)
            {
                resolve(true);
                return;
            }
            const t0 = Date.now();
            const tick = () =>
            {
                self.__waitTick = Date.now() - t0;
                if (!self.m_running)
                {
                    resolve(true);
                    return;
                }
                if (timeoutMs !== undefined && self.__waitTick > timeoutMs)
                {
                    resolve(false);
                    return;
                }
                setTimeout(tick, 10);
            };
            tick();
        });
    }

    static currentThreadId()
    {
        const api = workerApi();
        return (api && typeof api.selfId === "function") ? api.selfId() : 0;
    }
    static isMainThread()
    {
        const api = workerApi();
        return (api && typeof api.isMainThread === "function") ? !!api.isMainThread() : true;
    }
    static msleep(ms)
    {
        if (typeof globalThis.sleep === "function")
            globalThis.sleep((ms | 0) / 1000);
        else
            noteMissing("QThread::msleep(JS 侧请用 setTimeout/await)");
    }
}

// worker 之间没有共享内存(框架未启用 SAB/Atomics), 所以 QAtomicInteger 只是主线程内的计数器;
// 跨线程计数要走消息(postTo)或让 worker 自己维护。
class QAtomicInteger
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor(v) { this._v = v | 0; }
    load() { return this._v; }
    store(v) { this._v = v | 0; }
    fetchAndAdd(v) { const o = this._v; this._v += v; return o; }
    operator_plus_plus() { return this._v++; }
}

// ---------------------------------------------------------------------------
// 10. 消息框 / 对话框(无 GUI 时: 确定性默认返回值 + 记账, 绝不弹窗阻塞)
// ---------------------------------------------------------------------------
export class QMessageBox extends QObject
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor(parent) { super(parent); this._text = ""; this.sigFinished = []; }
    setText(t) { this._text = t; }
    text() { return this._text; }
    setInformativeText() { }
    setWindowTitle() { }
    addButton() { return makeLenient("QMessageBoxButton"); }
    exec()
    {
        noteMissing("QMessageBox::exec(无 GUI, 直接返回默认按钮)");
        return QMessageBox.Ok;
    }
    static information(a, b, c) { noteMissing("QMessageBox::information"); return QMessageBox.Ok; }
    static warning(a, b, c) { noteMissing("QMessageBox::warning"); return QMessageBox.Ok; }
    static critical(a, b, c) { noteMissing("QMessageBox::critical"); return QMessageBox.Ok; }
    static question(a, b, c, buttons) { noteMissing("QMessageBox::question"); return QMessageBox.Yes; }
    static about(a, b, c) { noteMissing("QMessageBox::about"); }
    static get Yes() { return 0x00004000; }
    static get No() { return 0x00010000; }
    static get Ok() { return 0x00000400; }
    static get Cancel() { return 0x00400000; }
    static get YesNo() { return 0x00004000 | 0x00010000; }
    static get OkCancel() { return 0x00000400 | 0x00400000; }
}

export class QFileDialog extends QObject
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor(parent) { super(parent); this.sigAccepted = []; }
    exec() { noteMissing("QFileDialog::exec(无 GUI)"); return 0; }
    setDirectory() { }
    setFilter() { }
    selectedFiles() { return []; }
    static getSaveFileName() { noteMissing("QFileDialog::getSaveFileName(无 GUI, 返回空 -> 调用方按取消处理)"); return ""; }
    static getOpenFileName() { noteMissing("QFileDialog::getOpenFileName(无 GUI, 返回空 -> 调用方按取消处理)"); return ""; }
    static getOpenFileNames() { noteMissing("QFileDialog::getOpenFileNames"); return []; }
    static getExistingDirectory() { noteMissing("QFileDialog::getExistingDirectory"); return ""; }
}

export class QInputDialog extends QObject
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor(parent) { super(parent); this.sigAccepted = []; }
    setLabelText() { }
    setTextValue() { }
    textValue() { return ""; }
    exec() { noteMissing("QInputDialog::exec(无 GUI)"); return 0; }
    static getText(a, b, c, d) { noteMissing("QInputDialog::getText"); return (d === undefined) ? "" : d; }
    static getItem(a, b, c, items, cur, editable) { noteMissing("QInputDialog::getItem"); return (items && items[0]) || ""; }
    static getDouble(a, b, c, v) { noteMissing("QInputDialog::getDouble"); return v || 0; }
    static getInt(a, b, c, v) { noteMissing("QInputDialog::getInt"); return v || 0; }
}

// ---------------------------------------------------------------------------
// 11. GUI 空壳: Proxy "宽容对象"
//
//   任何属性读取 -> 一个"万能函数"(可继续链式调用, 返回新的宽容对象)
//   任何以 sig 开头的属性 -> 回调数组(业务代码大量 push 信号)
//   任何构造/调用 -> 记账到 QtCompat.report()
// ---------------------------------------------------------------------------
// name: 记账用的路径(如 "QTableWidget.setColumnCount")
// quiet: 派生节点不再单独记账(只记顶层那一个, 避免 report 被深层路径刷屏)
// memberFactory: 可选 —— 给"按名字造真成员"用(见 Ui_*Class 的 uiMemberFactory)
function makeLenient(name, quiet, memberFactory)
{
    if (!quiet)
        noteMissing(name + " (空壳实例)");

    const store = Object.create(null);

    // 关键: target 必须是**函数**, 这样空壳对象既能被调用(万能方法), 又能继续取属性
    // (链式如 table.horizontalHeader().setStretchLastSection(true) 才不会断)。
    const target = function () { };

    const handler = {
        apply() { return makeLenient(name + "()", true); },
        construct() { return makeLenient(name + " (new)", true); },
        get(t, prop)
        {
            if (prop === "__qtstub") return name;
            if (prop === Symbol.toPrimitive) return () => 0;
            if (prop === "toString") return () => "[" + name + "]";
            if (prop === "valueOf") return () => 0;
            if (prop === "toJSON") return () => null;
            if (prop === "length") return 0;
            if (prop === "name") return name;
            if (prop in store) return store[prop];
            if (typeof prop === "string" && prop.slice(0, 3) === "sig")
            {
                store[prop] = [];                 // 业务代码大量 `sigXxx.push(cb)`
                return store[prop];
            }
            // 配对过真控件的空壳对象(见 asRealWidget): 控件方法转发到真控件
            const real = store.__jzWidget;
            if (real && FORWARD_METHODS.indexOf(prop) >= 0 && typeof real[prop] === "function")
            {
                store[prop] = function (...a) { return real[prop].apply(real, a); };
                return store[prop];
            }
            // 成员工厂(见 Ui_*Class): 按"名字约定"造真控件 —— ui.tableQuery 直接是 QTableWidget
            if (memberFactory && typeof prop === "string")
            {
                const made = memberFactory(prop);
                if (made !== undefined)
                {
                    store[prop] = made;
                    return made;
                }
            }
            // 其余属性一律再给一个"宽容对象"(本身可调用、可继续取属性), 并按需缓存
            store[prop] = makeLenient(name + "." + String(prop), true);
            return store[prop];
        },
        set(t, prop, v) { store[prop] = v; return true; },
        has() { return true; },
        deleteProperty(t, prop) { delete store[prop]; return true; },
        ownKeys() { return Object.keys(store); },
        getOwnPropertyDescriptor(t, prop) { return { configurable: true, enumerable: true, value: store[prop] }; },
    };
    return new Proxy(target, handler);
}

// 给"以 C++ 写法初始化基类的实例"挂一层兜底:
//
//   移植代码保留了 C++ 的 `QDialog.call(this, parent)` / `QWidget.call(this, parent)`,
//   而 `this` 是业务类(如 StepDataDialog)的普通实例 —— 它自己定义的方法都在, 但
//   Qt 基类那一大堆成员(windowFlags/setWindowFlags/setWindowTitle/...)不存在。
//   这里在实例的原型链上**插入一层代理**: 真实方法优先, 缺的成员给宽容空壳。
//   (不能直接给实例装代理 —— 那会把业务类自己的方法也吞掉。)
function attachQtFallback(obj, name)
{
    const proto = Object.getPrototypeOf(obj);
    if (!proto)
        return;

    const store = Object.create(null);
    const shim = new Proxy(proto, {
        get(t, prop)
        {
            const v = t[prop];                     // ① 业务类自己的成员优先
            if (v !== undefined)
                return v;
            // ② 配过真控件的(见 asRealWidget): 控件方法转发到真控件
            const fwd = forwardToRealWidget(obj, prop);
            if (fwd)
                return fwd;
            if (prop === Symbol.toPrimitive || prop === "valueOf") return () => 0;
            if (prop === "toString") return () => "[" + name + "]";
            if (prop === "constructor") return t.constructor;
            if (typeof prop === "string" && prop.slice(0, 3) === "sig")
            {
                if (!(prop in store)) store[prop] = [];
                return store[prop];
            }
            if (!(prop in store))
                store[prop] = makeLenient(name + "." + String(prop), true);
            return store[prop];
        },
        has() { return true; },
        set(t, prop, v) { t[prop] = v; return true; },
        // 让 instanceof 仍然成立: 查询"原型"时把插入的这层透过去
        getPrototypeOf(t) { return t; },
    });

    Object.setPrototypeOf(obj, shim);
    return shim;
}

// GUI 类: 构造/静态访问都宽容; 支持 `QWidget.call(this, parent)` 这种"以函数形式初始化"
//   成员当作选项传入
function makeQtStubClass(name, statics, memberFactory)
{
    const S = Object.assign({}, statics || {});
    const tokCache = Object.create(null);

    function ctor()
    {
        // 只有 QWidget.call(this, parent) 这类显式调用会走到这里
        if (this && typeof this === "object")
        {
            if (!this.__qt)
            {
                this.__qt = name;
                attachQtFallback(this, name);
            }
            return undefined;
        }
        noteMissing(name + " (空壳基类初始化)");
        return undefined;
    }

    const proxy = new Proxy(ctor, {
        // memberFactory 存在(Ui_*Class): 实例本身不是"缺口"(成员会按名字造成真控件) → 不记账
        construct() { return makeLenient(name, !!memberFactory, memberFactory); },
        get(t, prop)
        {
            // 函数自身成员(call/apply/bind/prototype/name/length/toString)优先
            const own = t[prop];
            if (own !== undefined)
                return own;
            if (prop in S)
                return S[prop];
            if (!(prop in tokCache))
            {
                noteMissing(name + "." + String(prop));
                // 稳定的记号对象: 比较合格(===)可用, 数值运算得 NaN(不崩)
                tokCache[prop] = name + "." + String(prop);
            }
            return tokCache[prop];
        },
        set(t, prop, v) { S[prop] = v; return true; },
        has() { return true; },
    });
    return proxy;
}

// ---------------------------------------------------------------------------
// 12. 全局注入
// ---------------------------------------------------------------------------
const QtCompat = {
    appDirPath,
    makeLenient,
    report,
    missing: g_missing,
    reset() { g_missing.clear(); },
    VERSION: "2026-09-13",
};

function report()
{
    if (g_missing.size === 0)
    {
        console.log("[qtcompat] 没有空壳调用被触发");
        return { ok: true, missing: [] };
    }
    const list = Array.from(g_missing.entries()).sort((a, b) => b[1] - a[1]);
    console.log("[qtcompat] 尚未实现(被调用到)的 Qt 能力, 共 " + list.length + " 项:");
    for (const [k, n] of list)
        console.log("  ×" + n + "  " + k);
    return { ok: false, missing: list };
}

// Qt 具名颜色回填
{
    const names = ["white", "black", "red", "darkRed", "green", "darkGreen", "blue", "darkBlue",
        "cyan", "darkCyan", "magenta", "darkMagenta", "yellow", "darkYellow", "gray", "darkGray",
        "lightGray", "orange"];
    for (const n of names)
        Qt[n] = new QColor(n);
}

const g_globals = {
    // 枚举 / 常量
    Qt, QtMsgType, QtDebugMsg, QtInfoMsg, QtWarningMsg, QtCriticalMsg, QtFatalMsg,
    NULL, TRUE, FALSE,
    SND_SYNC, SND_ASYNC, SND_NODEFAULT, SND_MEMORY, SND_LOOP, SND_FILENAME,
    EXCEPTION_ACCESS_VIOLATION,
    QIODevice,
    // 值类型
    QColor, QPoint, QSize, QRect, QPen, QBrush, QFont, QFontMetrics, QLinearGradient, QTextOption,
    // 文件 / 配置
    QFile, QDir, QTextStream, QSettings,
    // 对象 / 循环 / 应用
    QObject, QApplication, QTranslator, QSharedMemory, QThread, QAtomicInteger,
    // 对话框(半空壳)
    QMessageBox, QFileDialog, QInputDialog,
    // 小工具
    qRgb, qRgba,
    qMin: (a, b) => (a < b ? a : b),
    qMax: (a, b) => (a > b ? a : b),
    qRound: (v) => Math.round(v),
    qAbs: (v) => Math.abs(v),
    qSwap: (a, b) => b,
    qobject_cast: (x) => x,
    Q_ASSERT: (cond) => { if (!cond) console.error("[qtcompat] Q_ASSERT 失败"); },
    sizeof: () => 0,   // C++ sizeof; JS 侧无意义(业务改"序列化字节数"的占位)
    // Win32 直调
    PlaySoundW: (name, hmod, flags) =>
    {
        noteMissing("PlaySoundW(" + name + ") 未接音频后端");
        return FALSE;
    },
    RaiseException: (code, flags, nargs, args) =>
    {
        throw new Error("RaiseException(0x" + (code >>> 0).toString(16) + ")");
    },
    // Qt 消息处理器(引擎没有 Qt 的日志系统, 保留接线)
    qInstallMessageHandler: (fn) => { const old = g_msgHandler; g_msgHandler = fn; return old; },
    qRegisterMetaType: () => { },
};
let g_msgHandler = null;

// qApp: Qt 的全局应用单例(懒建)
Object.defineProperty(g_globals, "qApp", {
    enumerable: true,
    configurable: true,
    get() { return QApplication._inst || (QApplication._inst = new QApplication(0, [])); },
});

// GUI 空壳类清单: 引擎里没有的 Qt 控件/绘图/布局, 一律宽容空壳。
// 注意: 下面 12 节"框架桥"会把**引擎已注册**的那批名字换成真实现(见 frameworkBridgeReport),
// 这里就不列了; QTableWidgetItem/QListWidgetItem/QMenu/QAction/... 在下面有 JS 侧真定义。
const GUI_STUB_CLASSES = [
    // 窗口 / 控件
    "QTreeWidget", "QTreeWidgetItem", "QTableWidgetSelectionRange",
    "QToolButton", "QStatusBar", "QToolBar", "QMenuBar", "QStyledItemDelegate",
    "QTableWidgetHeader", "QFrame", "QAbstractButton", "QButtonGroup", "QDateTimeEdit", "QDateEdit",
    "QTimeEdit", "QCalendarWidget", "QTextBrowser", "QListView", "QAbstractSpinBox",
    "QDialogButtonBox", "QWizard", "QMdiArea", "QDockWidget", "QSplashScreen", "QSystemTrayIcon",
    "QDesktopWidget", "QApplicationWindow",
    // 布局
    "QLayout", "QBoxLayout", "QFormLayout", "QStackedLayout", "QSpacerItem",
    // 绘图
    "QIcon", "QGraphicsScene", "QGraphicsView", "QGraphicsItem",
    "QPainterPath", "QTransform", "QPolygon", "QRegion", "QCursor", "QMovie", "QSvgRenderer",
    // 事件
    "QEvent", "QMouseEvent", "QWheelEvent", "QKeyEvent", "QResizeEvent", "QPaintEvent",
    "QShowEvent", "QHideEvent", "QCloseEvent", "QEnterEvent", "QFocusEvent", "QContextMenuEvent",
    "QTimerEvent", "QDropEvent", "QDragEnterEvent", "QMoveEvent", "QChildEvent", "QMetaObject",
    // 校验 / 正则 / 其它
    "QValidator", "QDoubleValidator", "QIntValidator", "QRegExp", "QRegularExpression",
    "QUrl", "QDesktopServices", "QClipboard", "QMimeData", "QDrag", "QDropEventEx",
    // qcustomplot(第三方图表库, 未移植)
    "QCustomPlot", "QCPAxis", "QCPAxisRect", "QCPGraph", "QCPCurve", "QCPItemText", "QCPItemLine",
    "QCPItemTracer", "QCPAxisTickerTime", "QCPAxisTickerDateTime", "QCPLayoutGrid", "QCPLegend",
];

for (const name of GUI_STUB_CLASSES)
    g_globals[name] = makeQtStubClass(name, {});

// 视图枚举(Qt 里是 static 常量; 引擎没绑定 → 在 JS 侧补上, 值按 Qt 的原值)
g_globals.QAbstractItemView = makeQtStubClass("QAbstractItemView", {
    NoSelection: 0, SingleSelection: 1, MultiSelection: 2, ExtendedSelection: 3, ContiguousSelection: 4,
    SelectItems: 0, SelectRows: 1, SelectColumns: 2,
    NoEditTriggers: 0, CurrentChanged: 0, DoubleClicked: 2, SelectedClicked: 4, EditKeyPressed: 8,
    NoFocus: 0, TabFocus: 1, ClickFocus: 2, StrongFocus: 11, WheelFocus: 15,
});
g_globals.QHeaderView = makeQtStubClass("QHeaderView", {
    Interactive: 0, Stretch: 1, Fixed: 2, ResizeToContents: 3, Custom: 4,
    ResizeMode: 0, NoSection: 0, Horizontal: 1, Vertical: 2,
});
g_globals.QListView = makeQtStubClass("QListView", {
    ListMode: 0, IconMode: 1, SinglePass: 0, Batch: 1, Static: 0, Adjust: 2,
});

// ---------------------------------------------------------------------------
// 菜单 / 动作 / 快捷键: 框架**没有**这层绑定(无菜单栏/快捷键), 所以这里是 JS 侧真对象:
//   QActionGroup/QAction/QMenu 保存结构 + 状态(checked/enabled/text), 菜单 exec 在无 GUI 时
//   返回 null 并记账; QShortcut/QKeySequence 保存键位 + sigActivated 数组。
//   这样"动作/快捷键驱动的业务逻辑"可以照原样写, 只是菜单不弹、快捷键不注册(要触发就
//   调 action.trigger() / shortcut.activated())。
// ---------------------------------------------------------------------------
class QKeySequenceJS
{
    constructor(key) { this._key = (key === undefined || key === null) ? "" : String(key); }
    key() { return this._key; }
    toString() { return this._key; }
    isEmpty() { return this._key.length === 0; }
    static fromString(s) { return new QKeySequenceJS(s); }
}
class QShortcutJS
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor(keySeq, parent)
    {
        this._key = (keySeq && typeof keySeq.toString === "function") ? keySeq.toString() : String(keySeq === undefined ? "" : keySeq);
        this._parent = parent || null;
        this._enabled = true;
        this.sigActivated = [];
        this.sigActivatedAmbiguously = [];
    }
    key() { return this._key; }
    setKey(k) { this._key = (k && typeof k.toString === "function") ? k.toString() : String(k); }
    setEnabled(b) { this._enabled = !!b; }
    isEnabled() { return this._enabled; }
    setContext() { }
    setAutoRepeat() { return true; }
    // 手工触发(框架没有全局快捷键; 界面上的按钮/脚本可以借这个走同一条业务回调)
    activated() { emitSignal(this.sigActivated); }
    triggered() { this.activated(); }
}
class QActionJS
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor(textOrParent, parent)
    {
        this._text = (typeof textOrParent === "string") ? textOrParent : "";
        this._enabled = true;
        this._checkable = false;
        this._checked = false;
        this.sigTriggered = [];
        this.sigToggled = [];
    }
    text() { return this._text; }
    setText(t) { this._text = String(t); return this; }
    setEnabled(b) { this._enabled = !!b; return this; }
    isEnabled() { return this._enabled; }
    setDisabled(b) { return this.setEnabled(!b); }
    setCheckable(b) { this._checkable = !!b; return this; }
    isCheckable() { return this._checkable; }
    setChecked(b)
    {
        this._checked = !!b;
        if (this.sigToggled) emitSignal(this.sigToggled, this._checked);
        return this;
    }
    isChecked() { return this._checked; }
    toggle()
    {
        if (this._checkable) this.setChecked(!this._checked);
        emitSignal(this.sigTriggered, this._checked);
        return this;
    }
    trigger() { return this.toggle(); }
    setShortcut(s) { this._shortcut = s; noteMissing("QAction::setShortcut(框架没有快捷键绑定: 只记录键位)"); return this; }
    shortcut() { return this._shortcut || null; }
    setToolTip(t) { this._tip = t; return this; }
    toolTip() { return this._tip || this._text; }
    setStatusTip(t) { this._statusTip = t; return this; }
    setIcon(i) { this._icon = i; return this; }
    icon() { return this._icon || null; }
    setData(d) { this._data = d; return this; }
    data() { return this._data; }
    setVisible() { return this; }
    isVisible() { return true; }
    setSeparator() { this._separator = true; }
    isSeparator() { return !!this._separator; }
    setMenu(m) { this._menu = m; }
    menu() { return this._menu || null; }
    setObjectName(n) { this._name = n; }
    objectName() { return this._name || ""; }
}
class QActionGroupJS
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor(parent)
    {
        this._parent = parent || null;
        this._actions = [];
        this._exclusive = true;
    }
    addAction(a) { if (a) this._actions.push(a); return a; }
    removeAction(a)
    {
        const i = this._actions.indexOf(a);
        if (i >= 0) this._actions.splice(i, 1);
    }
    actions() { return this._actions.slice(); }
    setExclusive(b) { this._exclusive = !!b; }
    isExclusive() { return this._exclusive; }
    setEnabled(b) { for (let i = 0; i < this._actions.length; i++) this._actions[i].setEnabled(!!b); }
    checkedAction()
    {
        for (let i = 0; i < this._actions.length; i++)
            if (typeof this._actions[i].isChecked === "function" && this._actions[i].isChecked())
                return this._actions[i];
        return null;
    }
}
class QMenuJS
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor(titleOrParent, parent)
    {
        this._title = (typeof titleOrParent === "string") ? titleOrParent : "";
        this._entries = [];          // QAction 或 子 QMenu
        this._enabled = true;
    }
    title() { return this._title; }
    setTitle(t) { this._title = String(t); return this; }
    // Qt 的两个重载: addAction(text) 建新动作; addAction(action) 挂已有动作
    addAction(textOrAction)
    {
        if (textOrAction !== null && typeof textOrAction === "object")
        {
            this._entries.push(textOrAction);
            return textOrAction;
        }
        const a = new QActionJS(String(textOrAction === undefined ? "" : textOrAction));
        this._entries.push(a);
        return a;
    }
    addMenu(titleOrMenu)
    {
        const m = (titleOrMenu !== null && typeof titleOrMenu === "object") ? titleOrMenu : new QMenuJS(String(titleOrMenu === undefined ? "" : titleOrMenu));
        this._entries.push(m);
        return m;
    }
    addSeparator()
    {
        const a = new QActionJS("");
        a.setSeparator();
        this._entries.push(a);
        return a;
    }
    actions()
    {
        const out = [];
        for (let i = 0; i < this._entries.length; i++)
            if (!(this._entries[i] instanceof QMenuJS))
                out.push(this._entries[i]);
        return out;
    }
    isEmpty() { return this._entries.length === 0; }
    clear() { this._entries = []; }
    setEnabled(b) { this._enabled = !!b; }
    isEnabled() { return this._enabled; }
    setIcon() { }
    setTearOffEnabled() { }
    setSeparatorsCollapsible() { }
    // 无 GUI: 菜单弹不出来 —— 返回 null 并记账(业务里 `const act = menu.exec(pos)` 会走 null 分支)
    exec() { noteMissing("QMenu::exec(框架没有菜单弹出: 无 GUI 返回 null)"); return null; }
    popup() { return this.exec(); }
    showMenu() { return this.exec(); }
}
// QDateTimeEdit/QDateEdit/QTimeEdit: 框架没有日期控件绑定 → JS 侧真实现(内部就是 Date,
// 移植代码用的是 setDate(new Date()) / dateTime() / setDisplayFormat(...))。
function qtDateTimeFormat(d, fmt)
{
    const p = (v, n) => String(v).padStart(n, "0");
    return String(fmt === undefined || fmt === null ? "yyyy-MM-dd HH:mm:ss" : fmt)
        .replace(/yyyy/g, String(d.getFullYear()))
        .replace(/yy/g, p(d.getFullYear() % 100, 2))
        .replace(/MM/g, p(d.getMonth() + 1, 2))
        .replace(/dd/g, p(d.getDate(), 2))
        .replace(/HH/g, p(d.getHours(), 2))
        .replace(/hh/g, p((d.getHours() % 12) || 12, 2))
        .replace(/mm/g, p(d.getMinutes(), 2))
        .replace(/ss/g, p(d.getSeconds(), 2))
        .replace(/zzz/g, p(d.getMilliseconds(), 3));
}
class QDateTimeEditJS
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor(parentOrDate)
    {
        this._dt = (parentOrDate instanceof Date) ? new Date(parentOrDate.getTime()) : new Date();
        this._min = null;
        this._max = null;
    }
    setDate(d)
    {
        if (d instanceof Date)
        {
            const t = this._dt;
            this._dt = new Date(d.getFullYear(), d.getMonth(), d.getDate(),
                t.getHours(), t.getMinutes(), t.getSeconds(), t.getMilliseconds());
        }
        else if (typeof d === "string")
        {
            const p = d.split("-");
            if (p.length === 3) this.setDate(new Date(Number(p[0]), Number(p[1]) - 1, Number(p[2])));
        }
        return this;
    }
    date() { return new Date(this._dt.getFullYear(), this._dt.getMonth(), this._dt.getDate()); }
    setTime(d)
    {
        if (d instanceof Date)
        {
            const t = this._dt;
            this._dt = new Date(t.getFullYear(), t.getMonth(), t.getDate(),
                d.getHours(), d.getMinutes(), d.getSeconds());
        }
        return this;
    }
    time() { return new Date(1970, 0, 1, this._dt.getHours(), this._dt.getMinutes(), this._dt.getSeconds()); }
    setDateTime(d) { if (d instanceof Date) this._dt = new Date(d.getTime()); return this; }
    dateTime() { return new Date(this._dt.getTime()); }
    setMinimumDate(d) { this._min = (d instanceof Date) ? new Date(d.getTime()) : null; }
    minimumDate() { return this._min ? new Date(this._min.getTime()) : null; }
    setMaximumDate(d) { this._max = (d instanceof Date) ? new Date(d.getTime()) : null; }
    maximumDate() { return this._max ? new Date(this._max.getTime()) : null; }
    setDisplayFormat(f) { this._fmt = String(f); return this; }
    displayFormat() { return this._fmt || "yyyy-MM-dd HH:mm:ss"; }
    setCalendarPopup() { }
    setTimeSpec() { }
    setReadOnly() { }
    setEnabled(b) { this._enabled = !!b; }
    isEnabled() { return this._enabled !== false; }
    setVisible() { }
    setObjectName(n) { this._name = n; }
    objectName() { return this._name || ""; }
    toString(f) { return qtDateTimeFormat(this._dt, f === undefined ? this._format : f); }
    setStyleSheet() { }
    clear() { this._dt = new Date(); }
}
g_globals.QDateTimeEdit = QDateTimeEditJS;
g_globals.QDateEdit = QDateTimeEditJS;
g_globals.QTimeEdit = QDateTimeEditJS;
g_globals.QKeySequence = QKeySequenceJS;
g_globals.QShortcut = QShortcutJS;
g_globals.QAction = QActionJS;
g_globals.QActionGroup = QActionGroupJS;
g_globals.QMenu = QMenuJS;

// 控件基类需要接受 `QWidget.call(this, parent)` 语义, 上面的工厂已支持。
//
// Ui_*Class: uic 从 .ui 生成的界面类。仓库里**没有** .ui 文件, 也就没有对应的 JS,
// 但每个窗口/对话框的构造函数第一句都是 `this.ui = new Ui_XxxClass()`, 缺一个就
// 整个界面层都构造不起来。这里按代码里实际引用的名字统一给空壳, 让整机装配可以跑通
// (真正的控件树仍然等 .ui → Ui_*Class 的移植)。
const UI_STUB_CLASSES = [
    "Ui_BTEServerClass",
    "Ui_DeviceMacDialogClass",
    "Ui_HelpDialogClass",
    "Ui_SplitDialogClass",
    "Ui_SplitSettingWidgetClass",
    "Ui_StepDataDialogClass",
    "Ui_StepEditDialogClass",
    "Ui_StepInfoDialogClass",
    "Ui_StepJumpDialogClass",
    "Ui_StepSettingDialogClass",
];
for (const name of UI_STUB_CLASSES)
    g_globals[name] = makeQtStubClass(name, {});

// ---------------------------------------------------------------------------
// 12. 框架桥: Qt 名字 -> 框架(JZ*)的**真实现**
//
//   引擎的 JZJSWidgetModule 把框架的控件/值类型注册成 JS 全局类(可直接 new):
//     JZWidget JZWindow JZLabel JZPushButton JZLineEdit JZCheckbox JZRadioButton JZSlider
//     JZProgressBar JZComboBox JZSpinBox JZGroupBox JZChart
//     JZLayout JZVBoxLayout JZHBoxLayout JZGridLayout JZSelfLayout
//     JZPoint JZSize JZRect JZColor JZImage JZPainter (+ 事件类)
//   本节把同名的 Qt 类型换成它们(子类 + 补 Qt 的构造签名/方法名), 业务代码不用改。
//   框架**没有注册到 JS** 的那几个(JZTableWidget/JZListWidget/JZMenu/JZMessageBox/
//   JZFont/JZPen/JZBrush/JZSettings/JZThread —— C++ 侧都有类, 只是没绑定)继续用
//   本文件的 JS 实现, 见 frameworkBridgeReport() 的清单。
//
//   为什么用子类而不是直接 alias:
//     (a) Qt 的构造签名与框架不同: new QLabel("text", parent) / new QPoint(x, y)
//         (jsbind 的默认构造**不转发参数**, 得靠 setXxx 补);
//     (b) Qt 的方法名不同: setAlignment / setWindowTitle / setFixedSize(QRect) ...;
//     (c) 本移植的信号约定是"回调数组"(sigClicked.push(cb)), 而框架是 onClicked(cb)。
// ---------------------------------------------------------------------------
export const frameworkBridge = { real: [], js: [] };

function jzType(name)
{
    const v = globalThis[name];
    return (typeof v === "function") ? v : null;
}

function emitSignal(list, ...args)
{
    if (!Array.isArray(list))
        return;
    for (let i = 0; i < list.length; i++)
    {
        try { list[i].apply(null, args); }
        catch (e) { console.error("[qtcompat] 信号回调异常: " + e); }
    }
}

// 每个控件实例一份"信号数组"。jsbind 的原生实例上不一定能挂普通 JS 属性, 所以放 WeakMap,
// 再用原型 getter 暴露成 `obj.sigClicked`(业务代码是 `sigClicked.push(cb)`, 兼容)。
const g_sigStore = new WeakMap();
const SIGNAL_NAMES = ["sigClicked", "sigTextChanged", "sigCurrentIndexChanged", "sigValueChanged",
    "sigStateChanged", "sigEditingFinished", "sigClose", "sigLinkActivated",
    // 移植代码实际 push 过的信号名(业务是 sigXxx.push(cb), 见 grep 统计)
    "sigCustomContextMenuRequested", "sigToggled", "sigActivated", "sigCurrentTextChanged",
    "sigCurrentChanged", "sigSplitterMoved", "sigItemSelectionChanged", "sigCellClicked",
    "sigDoubleClicked", "sigReturnPressed", "sigPrepareData", "sigSplitFinish",
    "sigMessage", "sigFinished", "sigChannelChanged", "sigProgress", "sigStateChangedEx"];

function sigsOf(obj)
{
    let m = g_sigStore.get(obj);
    if (!m)
    {
        m = {};
        for (let i = 0; i < SIGNAL_NAMES.length; i++)
            m[SIGNAL_NAMES[i]] = [];
        g_sigStore.set(obj, m);
    }
    return m;
}

function installSignals(Base)
{
    for (let i = 0; i < SIGNAL_NAMES.length; i++)
    {
        const n = SIGNAL_NAMES[i];
        if (Object.getOwnPropertyDescriptor(Base.prototype, n))
            continue;                      // 已装过(同一原型被多个 Qt 名字共用)
        Object.defineProperty(Base.prototype, n, {
            get() { return sigsOf(this)[n]; },
            set(v) { sigsOf(this)[n] = v; },
            configurable: true,
        });
    }
}

// 框架原型上的**原始**方法表(被 Qt 同名方法覆盖前先存下来, 供适配层回调)
const g_orig = new WeakMap();

function origJz(jzName, name)
{
    const Base = jzType(jzName);
    const m = Base ? g_orig.get(Base) : null;
    return (m && m[name]) ? m[name] : null;
}

// Qt 名字 -> 框架真类。三个关键决定:
//   1) **不能**用 `class QLabel extends JZLabel`: jsbind 的构造返回的是原生包装对象,
//      派生类的 prototype 会被丢掉(Qt 方法全丢, `.normalized 不是函数`就是这么来的)。
//      所以 Qt 的方法名直接补到**框架原型**上 —— 实例就是框架对象, 方法一定在。
//   2) Qt 的构造签名("new QLabel('文本')"/"new QPoint(3,4)")用"工厂函数 + 共享原型"
//      实现: 调用者拿到的仍是真框架对象, 且 `instanceof` 两边都成立。
//   3) 兼容 C++ 的 `QDialog.call(this, parent)` 基类初始化写法(Proxy apply -> 兜底层)。
function bridgeJz(qtName, jzName, ctor, proto)
{
    const Base = jzType(jzName);
    if (!Base)
        return false;

    if (!g_orig.has(Base))
    {
        // 必须用 getOwnPropertyNames: jsbind 注册的方法**不可枚举**(for-in 拿不到),
        // 用 for-in 建出来的快照是空的, 适配层回调原实现时会变成静默 no-op。
        const snap = {};
        const names = Object.getOwnPropertyNames(Base.prototype);
        for (let i = 0; i < names.length; i++)
            snap[names[i]] = Object.getOwnPropertyDescriptor(Base.prototype, names[i]).value;
        g_orig.set(Base, snap);
    }

    if (proto)
    {
        for (const k in proto)
            Base.prototype[k] = proto[k];  // 补/覆盖 Qt 方法名
    }
    installSignals(Base);

    function QtCtor(...args)
    {
        const obj = new Base();
        ctor.apply(obj, args);
        return obj;
    }
    QtCtor.prototype = Base.prototype;
    Object.setPrototypeOf(QtCtor, Base);   // 继承静态成员(如 JZChart.LineChart)

    const P = new Proxy(QtCtor, {
        apply(target, thisArg, args)
        {
            if (thisArg !== undefined && thisArg !== null && typeof thisArg === "object")
            {
                // `QDialog.call(this, parent)`: 退化成"缺啥补啥"的原型层(与空壳路径一致)
                attachQtFallback(thisArg, jzName);
                return undefined;
            }
            return target(...args);        // 普通调用: 当工厂用
        },
    });
    P.jzBase = jzName;                     // 便于自检/调试: 这个 Qt 名字背后是哪个框架类
    g_globals[qtName] = P;
    frameworkBridge.real.push(qtName + " <- " + jzName);
    return true;
}

// 布局里可能出现"还不是真控件"的 JS 对象(BTEItemWidget 这类仍用 QWidget.call(this) 的写法),
// 框架的 addWidget 有类型校验, 会直接抛"参数1需要类型JZWidget"。
// 处理办法 = **配对**: 给这个 JS 对象配一个真 JZWidget, 并把
//   - 控件的常用方法(显示/几何/刷新/样式)转发到真控件;
//   - 绘制/鼠标/尺寸钩子接回 JS 对象的 Qt 方法(paintEvent/mousePressEvent/...)。
// 于是"JS 对象当控件用"的代码也能驱动真控件 —— 这是 BTEItemWidget 这类
// "C++ 基类初始化写法" 的类不必整体重写就能变真的关键一步。
const g_jzWidgetOf = new WeakMap();   // JS 对象 -> 它对应的真 JZWidget
const FORWARD_METHODS = ["setVisible", "show", "hide", "setGeometry", "move", "resize",
    "setFixedSize", "setFixedWidth", "setFixedHeight", "setMinimumSize", "setMaximumSize",
    "update", "setStyleSheet", "setTooltip", "setParentWidget", "setFocus",
    "setContextMenuPolicy", "setEnabled", "rect", "size", "pos", "mapTo", "mapFrom", "repaint"];

// ---------------------------------------------------------------------------
// Qt 事件 -> 框架事件对象的包装
//   框架给的是 JZMouseEvent(localX/localY/globalX/globalY/button/wheelDelta/modifiers),
//   而移植代码是 Qt 写法(event.pos()/event.globalPos()/event.button()/event.accept())。
//   这里按需包一层, 不改业务代码。绘制事件不用包: JZPainterEvent.rect() 与 Qt 同名。
// ---------------------------------------------------------------------------
function qtMouseEvent(e)
{
    if (!e || typeof e !== "object")
        return e;
    return {
        _raw: e,
        pos: () => new g_globals.QPoint(e.localX ? e.localX() : 0, e.localY ? e.localY() : 0),
        globalPos: () => new g_globals.QPoint(e.globalX ? e.globalX() : 0, e.globalY ? e.globalY() : 0),
        localPos: () => new g_globals.QPoint(e.localX ? e.localX() : 0, e.localY ? e.localY() : 0),
        x: () => (e.localX ? e.localX() : 0),
        y: () => (e.localY ? e.localY() : 0),
        button: () => (e.button ? e.button() : 0),
        buttons: () => (e.button ? e.button() : 0),
        modifiers: () => (e.modifiers ? e.modifiers() : 0),
        accept: () => { if (e.accept) e.accept(); },
        ignore: () => { if (e.ignore) e.ignore(); },
        isAccepted: () => (e.isAccepted ? e.isAccepted() : true),
    };
}

function qtWheelEvent(e)
{
    if (!e || typeof e !== "object")
        return e;
    const m = qtMouseEvent(e);
    m.angleDelta = () => new g_globals.QPoint(0, (e.wheelDelta ? e.wheelDelta() : 0));
    m.delta = () => (e.wheelDelta ? e.wheelDelta() : 0);
    return m;
}

function qtKeyEvent(e)
{
    if (!e || typeof e !== "object")
        return e;
    return {
        _raw: e,
        key: () => (e.key ? e.key() : 0),
        text: () => (e.text ? e.text() : ""),
        modifiers: () => (e.modifiers ? e.modifiers() : 0),
        isAutoRepeat: () => (e.isRepeat ? !!e.isRepeat() : false),
        accept: () => { if (e.accept) e.accept(); },
        ignore: () => { if (e.ignore) e.ignore(); },
    };
}

// 真控件的"绘制/交互"钩子 -> target 上的 Qt 风格方法(paintEvent/mousePressEvent/...)
//   target 省略 = 真控件自己(真控件化的类); 给了 target = 配对的 JS 对象(asRealWidget)。
function installWidgetHooks(real, target)
{
    const T = target || real;
    const hook = (jzName, qtName, wrap) =>
    {
        real[jzName] = (e) =>
        {
            const fn = T[qtName];
            if (typeof fn !== "function")
                return;
            try { fn.call(T, wrap ? wrap(e) : e); }
            catch (ex) { console.error("[qtcompat] " + qtName + " 异常: " + ex); }
        };
    };
    hook("onPaint", "paintEvent", null);
    hook("onMousePress", "mousePressEvent", qtMouseEvent);
    hook("onMouseRelease", "mouseReleaseEvent", qtMouseEvent);
    hook("onMouseMove", "mouseMoveEvent", qtMouseEvent);
    hook("onWheel", "wheelEvent", qtWheelEvent);
    hook("onResize", "resizeEvent", null);
    hook("onKeyPress", "keyPressEvent", qtKeyEvent);
    hook("onKeyRelease", "keyReleaseEvent", qtKeyEvent);
    hook("onEnter", "enterEvent", null);
    hook("onLeave", "leaveEvent", null);
    hook("onShow", "showEvent", null);
    hook("onHide", "hideEvent", null);
    hook("onClose", "closeEvent", null);
}

// ---------------------------------------------------------------------------
// 真控件化: `new BTEItemWidget()` 直接得到一个**真 JZWidget**
//
//   不能用 `class BTEItemWidget extends JZWidget`: jsbind 的构造返回原生包装对象,
//   派生原型会被丢掉(类方法全丢)。所以约定写成:
//
//       class BTEItemWidget
//       {
//           constructor(parent) { return makeRealWidget(this, JZWidget, parent); }
//           initWidget(parent) { ...原来的构造逻辑, this = 真控件... }
//           ...业务方法...
//       }
//
//   makeRealWidget 做四件事: 建真控件 -> 把类原型上的方法/访问器挂到实例 ->
//   接上绘制/交互钩子 -> 调 initWidget(如果有)。
// ---------------------------------------------------------------------------
const g_realWidgetClasses = [];      // 已"真控件化"的类名(进框架桥报告)

function installClassMembers(Cls, real)
{
    const names = Object.getOwnPropertyNames(Cls.prototype);
    for (let i = 0; i < names.length; i++)
    {
        const k = names[i];
        if (k === "constructor")
            continue;
        const d = Object.getOwnPropertyDescriptor(Cls.prototype, k);
        if (!d)
            continue;
        try
        {
            if (d.get || d.set)
                Object.defineProperty(real, k, d);
            else if (typeof d.value === "function")
                real[k] = d.value;
            else
                real[k] = d.value;
        }
        catch (e)
        {
            console.error("[qtcompat] 真控件化: 无法把 " + Cls.name + "." + k + " 挂到实例: " + e);
        }
    }
}

// 真控件化入口挂成全局: 业务文件里写 `makeRealWidget(this, JZWidget, parent)` 即可
g_globals.makeRealWidget = makeRealWidget;

export function makeRealWidget(self, Jz, ...args)
{
    const Cls = (self && self.constructor && self.constructor !== Object) ? self.constructor : null;
    const real = new Jz();
    if (Cls)
    {
        installClassMembers(Cls, real);
        if (g_realWidgetClasses.indexOf(Cls.name) < 0)
            g_realWidgetClasses.push(Cls.name);
    }
    installWidgetHooks(real, real);
    if (typeof real.initWidget === "function")
        real.initWidget.apply(real, args);
    return real;
}

function asRealWidget(w)
{
    if (w === undefined || w === null)
        return null;
    if (typeof JZWidget === "function" && (w instanceof JZWidget))
        return w;

    let real = g_jzWidgetOf.get(w);
    if (real)
        return real;

    let nm = "object";
    try { nm = (w && w.constructor && w.constructor.name) ? String(w.constructor.name) : String(w); }
    catch (e) { nm = "<unknown>"; }
    noteMissing("布局里的非框架控件(仍是 JS 对象, 已配对真控件占位): " + nm);

    real = new JZWidget();
    g_jzWidgetOf.set(w, real);
    try { w.__jzWidget = real; } catch (e) { /* 有的对象不让挂属性 */ }
    installWidgetHooks(real, w);   // 钩子接回那个 JS 对象的 Qt 方法
    return real;
}

// 给"JS 对象上的控件方法"转发到它配对的真控件(由 attachQtFallback / makeLenient 的 get 调用)
function forwardToRealWidget(obj, prop)
{
    if (FORWARD_METHODS.indexOf(prop) < 0)
        return undefined;
    const real = (obj && obj.__jzWidget) ? obj.__jzWidget : g_jzWidgetOf.get(obj);
    if (!real || typeof real[prop] !== "function")
        return undefined;
    return function (...args) { return real[prop].apply(real, args); };
}

function noop() { }

// Qt 的颜色字符串: "#RGB" / "#RRGGBB" / "#AARRGGBB" / 具名色
function parseColorSpec(s)
{
    const t = String(s).trim();
    if (t.length > 1 && t[0] === "#")
    {
        const h = t.slice(1);
        const hx = (x) => parseInt(x, 16) || 0;
        if (h.length === 3)
            return [hx(h[0] + h[0]), hx(h[1] + h[1]), hx(h[2] + h[2]), 255];
        if (h.length === 6)
            return [hx(h.slice(0, 2)), hx(h.slice(2, 4)), hx(h.slice(4, 6)), 255];
        if (h.length === 8)
            return [hx(h.slice(2, 4)), hx(h.slice(4, 6)), hx(h.slice(6, 8)), hx(h.slice(0, 2))];
    }
    const c = parseColorName(t);
    return [c[0], c[1], c[2], 255];
}

// ---- 值类型(框架的 JZPoint/JZSize/JZRect/JZColor 就是真实现) ----
bridgeJz("QPoint", "JZPoint",
    function (x, y)
    {
        if (x !== undefined)
            this.setPoint(x | 0, (y === undefined ? x : y) | 0);
    },
    {
        isNull: function () { return this.x() === 0 && this.y() === 0; },
        rx: function () { return this.x(); },
        ry: function () { return this.y(); },
        equals: function (o) { return !!o && this.x() === o.x() && this.y() === o.y(); },
        toString: function () { return this.x() + "," + this.y(); },
    });

bridgeJz("QSize", "JZSize",
    function (w, h)
    {
        if (w !== undefined)
        {
            this.setWidth(w | 0);
            this.setHeight((h === undefined ? w : h) | 0);
        }
    },
    {
        setSize: function (w, h) { this.setWidth(w | 0); this.setHeight(h | 0); },
        toString: function () { return this.width() + "x" + this.height(); },
    });

bridgeJz("QRect", "JZRect",
    function (a, b, c, d)
    {
        if (a === undefined)
            return;
        if (a !== null && typeof a === "object" && typeof a.x === "function")
        {
            if (b !== null && typeof b === "object" && typeof b.x === "function")
            {   // QRect(QPoint, QPoint)
                const x1 = a.x(), y1 = a.y(), x2 = b.x(), y2 = b.y();
                this.setRect(Math.min(x1, x2), Math.min(y1, y2),
                    Math.abs(x2 - x1) + 1, Math.abs(y2 - y1) + 1);
            }
            else
            {   // QRect(QPoint, QSize)
                this.setRect(a.x() | 0, a.y() | 0,
                    (typeof b === "number" ? b : 0) | 0, (typeof c === "number" ? c : 0) | 0);
            }
            return;
        }
        this.setRect(a | 0, b | 0, (c === undefined ? 0 : c) | 0, (d === undefined ? 0 : d) | 0);
    },
    {
        // Qt 语义: 保证 left<=right / top<=bottom
        normalized: function ()
        {
            const l = this.left(), t = this.top(), r = this.right(), b = this.bottom();
            return new g_globals.QRect(Math.min(l, r), Math.min(t, b),
                Math.abs(r - l) + 1, Math.abs(b - t) + 1);
        },
        adjusted: function (dx1, dy1, dx2, dy2)
        {
            return new g_globals.QRect(this.left() + dx1, this.top() + dy1,
                this.width() + dx2 - dx1, this.height() + dy2 - dy1);
        },
        // Qt 有 contains(QPoint) 重载; 框架的 JZRect::contains 只吃 (x,y)
        contains: function (a, b)
        {
            const f = origJz("JZRect", "contains");
            if (!f)
                return false;
            if (a !== null && typeof a === "object" && typeof a.x === "function")
                return f.call(this, a.x(), a.y());
            return f.call(this, a, b);
        },
        setCoords: function (x1, y1, x2, y2) { this.setRect(x1, y1, x2 - x1 + 1, y2 - y1 + 1); },
        getRect: function () { return [this.x(), this.y(), this.width(), this.height()]; },
        isValid: function () { return !this.isEmpty(); },   // 框架的 JZRect 没有 isValid
        moveLeft: function (x) { this.setRect(x, this.top(), this.width(), this.height()); },
        moveTop: function (y) { this.setRect(this.left(), y, this.width(), this.height()); },
    });

bridgeJz("QColor", "JZColor",
    function (r, g, b, a)
    {
        if (r === undefined)
        {   // Qt 默认构造是"无效颜色", 这里给黑色(业务只在临时占位时用到)
            this.setRgb(0, 0, 0);
            return;
        }
        if (typeof r === "object" && r !== null && typeof r.red === "function")
        {   // QColor(QColor): 拷贝
            this.setRgba(r.red() | 0, r.green() | 0, r.blue() | 0, r.alpha() | 0);
            return;
        }
        if (typeof r === "string")
        {
            const c = parseColorSpec(r);
            this.setRgba(c[0], c[1], c[2], c[3]);
            return;
        }
        if (g === undefined)
        {   // QColor(QRgb)
            const v = r >>> 0;
            this.setRgba((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff, ((v >>> 24) & 0xff) || 255);
            return;
        }
        this.setRgba(r | 0, g | 0, b | 0, (a === undefined ? 255 : a | 0));
    },
    {
        setRgb: function (r, g, b) { this.setRgba(r | 0, g | 0, b | 0, 255); },
        rgb: function ()
        {
            return ((255 << 24) | (this.red() << 16) | (this.green() << 8) | this.blue()) >>> 0;
        },
        rgba: function ()
        {
            return ((this.alpha() << 24) | (this.red() << 16) | (this.green() << 8) | this.blue()) >>> 0;
        },
        name: function ()
        {
            return "#" + [this.red(), this.green(), this.blue()]
                .map((v) => (((v | 0) & 0xff).toString(16).padStart(2, "0"))).join("");
        },
        isValid: function () { return true; },
        equals: function (o)
        {
            return !!o && this.red() === o.red() && this.green() === o.green()
                && this.blue() === o.blue() && this.alpha() === o.alpha();
        },
        toString: function () { return this.name(); },
        // 与 CSS 互通(框架画笔吃 CSS 颜色串)
        css: function () { return this.name(); },
    });

// ---- 控件 ----
// Qt 里窗口类: JZWindow 是真窗口, 补 Qt 的窗口 API
function windowProtoCtor(titleOrParent, parent)
{
    this._flags = 0;
    this.sigClose = [];
    if (typeof titleOrParent === "string")
        this.setTitle(titleOrParent);
}
const WINDOW_PROTO = {
    setWindowTitle: function (t) { this.setTitle(t); },
    setWindowFlags: function (f) { this._flags = f >>> 0; },
    windowFlags: function () { return this._flags; },
    setModal: function () { },
    setAttribute: function () { },
    setContentsMargins: function () { },
    adjustSize: noop,
    setCentralWidget: function (w) { if (w) this.addWidget(w); },
    centralWidget: function () { return null; },
    isMaximized: function () { return false; },
    isVisible: function () { return true; },
};
bridgeJz("QMainWindow", "JZWindow", windowProtoCtor, WINDOW_PROTO);
bridgeJz("QDialog", "JZWindow", windowProtoCtor, WINDOW_PROTO);

// 每个控件都要补: Qt 的 setFixedSize(QRect) 形式 + 本移植的"信号数组"约定
function widgetProtoCtor()
{
    sigsOf(this);          // 就地建好各信号数组(之后由原型 getter 取)
    this._flags = 0;
}
const WIDGET_PROTO = {
    setFixedSize: function (a, b)   // Qt 有 setFixedSize(w,h) 与 setFixedSize(QSize) 两种
    {
        const fn = origJz("JZWidget", "setFixedSize");
        if (!fn)
            return;
        if (a !== null && typeof a === "object" && typeof a.width === "function")
            return fn.call(this, a.width(), a.height());
        return fn.call(this, a, b);
    },
    setToolTip: function (t) { this.setTooltip(t); },
    setEnabled: function (b) { this._enabled = !!b; },     // 框架没有 setEnabled -> JS 侧状态
    isEnabled: function () { return this._enabled !== false; },
    setObjectName: function (n) { this._name = n; },
    objectName: function () { return this._name || ""; },
    // Qt 的 setFont/setPalette: 框架控件没有字体 API(UI 上的字体走 .ui/QSS)
    setFont: function (f) { this._font = f; },
    font: function () { return this._font || new g_globals.QFont(); },

    // ---- Qt 里由 QWidget 提供、框架没有的成员 ----
    rect: function () { return new g_globals.QRect(0, 0, this.width(), this.height()); },
    size: function () { return new g_globals.QSize(this.width(), this.height()); },
    pos: function () { return new g_globals.QPoint(this.x(), this.y()); },
    // 控件坐标 -> 祖先坐标: 沿 parentWidget 累加(框架没有 mapTo)
    //   注意: 顶层控件的 parentWidget() 不能拿来做"再取 x()"这类操作(会崩), 所以这里
    //   只用 instanceof 判断父级是不是控件(纯 JS 原型链判断, 不碰 C++ 指针), 拿不准就停。
    mapTo: function (ancestor, p)
    {
        let x = p ? p.x() : 0, y = p ? p.y() : 0;
        let w = this;
        for (let guard = 0; guard < 64; guard++)
        {
            if (!w || w === ancestor)
                break;
            if (typeof w.x === "function") { x += w.x(); y += w.y(); }
            let up = null;
            try { up = (typeof w.parentWidget === "function") ? w.parentWidget() : null; }
            catch (e) { break; }
            if (!up || up === w)
                break;
            if (typeof JZWidget === "function" && !(up instanceof JZWidget))
                break;
            w = up;
        }
        return new g_globals.QPoint(x, y);
    },
    mapFrom: function (from, p)
    {
        if (from && typeof from.mapTo === "function")
            return from.mapTo(this, p);
        return new g_globals.QPoint(p ? p.x() : 0, p ? p.y() : 0);
    },
    setContextMenuPolicy: function (p) { this._ctxPolicy = p | 0; },
    contextMenuPolicy: function () { return this._ctxPolicy || 0; },
    setMouseTracking: function (b) { this._mouseTracking = !!b; },
    hasMouseTracking: function () { return !!this._mouseTracking; },
    setCursor: function (c) { this._cursor = c; },
    cursor: function () { return this._cursor; },
    setSizePolicy: noop, setMinimumWidth: noop, setMinimumHeight: noop,
    setMaximumWidth: noop, setMaximumHeight: noop, setAutoFillBackground: noop,
    setBackgroundRole: noop, setWindowOpacity: noop, setWindowState: noop,
    setAttribute: noop, setAcceptDrops: noop,
    setProperty: function (n, v) { this["__qtprop_" + n] = v; },
    property: function (n) { return this["__qtprop_" + n]; },
    grab: function () { return null; },
    repaint: function () { if (typeof this.update === "function") this.update(); },
    // 坐标转换: 框架没有全局屏幕坐标, 只能"原样返回"(记账一次, 菜单/工具提示定位不受影响)
    mapToGlobal: function (p)
    {
        noteMissing("QWidget::mapToGlobal(框架无屏幕坐标, 原样返回)");
        return (p && typeof p.x === "function") ? p : new g_globals.QPoint(0, 0);
    },
    mapFromGlobal: function (p) { return (p && typeof p.x === "function") ? p : new g_globals.QPoint(0, 0); },
    mapToParent: function (p) { return this.mapTo(this.parentWidget ? this.parentWidget() : null, p); },
    blockSignals: function (b) { this._blockSignals = !!b; },
    signalsBlocked: function () { return !!this._blockSignals; },
    setUpdatesEnabled: noop, setFocusPolicy: noop, setTabOrder: noop,
    setContentsMargins: noop, setSizeIncrement: noop, setBaseSize: noop,
    setAutoFillBackground: noop, setUpdatesEnabledFlag: noop,
    window: function () { return null; },
    isActiveWindow: function () { return false; },
    grabKeyboard: noop, releaseKeyboard: noop,
    fontMetrics: function () { return (typeof g_globals.QFontMetrics === "function") ? new g_globals.QFontMetrics(this._font) : null; },
    palette: function () { return makeLenient("QWidget::palette", true); },

    // ---- 虚函数形状: 框架没有这些方法, 但移植代码会写
    //      `QWidget.prototype.mousePressEvent.call(this, event)`(调基类实现) → 给 no-op;
    //      真控件化的类会把实例上的同名方法盖在上面(installClassMembers)。
    paintEvent: noop, mousePressEvent: noop, mouseReleaseEvent: noop, mouseMoveEvent: noop,
    mouseDoubleClickEvent: noop, wheelEvent: noop, resizeEvent: noop, keyPressEvent: noop,
    keyReleaseEvent: noop, showEvent: noop, hideEvent: noop, closeEvent: noop,
    enterEvent: noop, leaveEvent: noop, contextMenuEvent: noop, changeEvent: noop,
};

// QWidget: 所有控件的基类。Qt 的方法名统一补到 JZWidget.prototype 上(子类继承),
// 这样"真控件化"出来的实例(直接 new JZWidget)也带着完整 Qt 成员。
bridgeJz("QWidget", "JZWidget",
    function (parent) { this._flags = 0; },
    Object.assign({}, WIDGET_PROTO, {
        setWindowTitle: noop,
        setWindowFlags: function (f) { this._flags = f >>> 0; },
        windowFlags: function () { return this._flags; },
        setModal: noop,
    }));

// 名字像 A、用起来是 B 的控件: 业务里 box*/line* 既有复选框/编辑框又有下拉框/数值框
// (如 ui.boxDevice 是 ComboBox, ui.lineLoopTime 是 SpinBox)。桥给它们**兄弟协议**:
// 缺失的那半边用 JS 侧状态顶住(记账一次), 于是两种用法都不崩、逻辑也走得通。
const SIBLING_COMBO_METHODS = {
    addItem: function (t)
    {
        this._comboItems = this._comboItems || [];
        this._comboItems.push(String(t));
        if (this._comboIndex === undefined || this._comboIndex < 0) this._comboIndex = 0;
        noteMissing("控件当 ComboBox 用(addItem)");
    },
    addItems: function (arr)
    {
        this._comboItems = this._comboItems || [];
        for (let i = 0; arr && i < arr.length; i++) this._comboItems.push(String(arr[i]));
        if (this._comboIndex === undefined || this._comboIndex < 0) this._comboIndex = 0;
        noteMissing("控件当 ComboBox 用(addItems)");
    },
    clear: function () { this._comboItems = []; this._comboIndex = -1; },
    count: function () { return (this._comboItems || []).length; },
    itemText: function (i) { return (this._comboItems || [])[i | 0] || ""; },
    currentIndex: function () { return (this._comboIndex === undefined) ? -1 : this._comboIndex; },
    setCurrentIndex: function (i) { this._comboIndex = i | 0; noteMissing("控件当 ComboBox 用(setCurrentIndex)"); },
    currentText: function ()
    {
        const a = this._comboItems || [];
        const i = (this._comboIndex === undefined) ? -1 : this._comboIndex;
        return (i >= 0 && i < a.length) ? a[i] : "";
    },
    setCurrentText: function (t)
    {
        const a = this._comboItems || [];
        const i = a.indexOf(String(t));
        if (i >= 0) this._comboIndex = i;
        noteMissing("控件当 ComboBox 用(setCurrentText)");
    },
    onCurrentIndexChanged: function (cb) { (this.sigCurrentIndexChanged || (this.sigCurrentIndexChanged = [])).push(cb); },
    onCurrentTextChanged: function (cb) { (this.sigCurrentTextChanged || (this.sigCurrentTextChanged = [])).push(cb); },
};
const SIBLING_CHECK_METHODS = {
    isChecked: function () { if (typeof this.__nativeIsChecked === "function") return this.__nativeIsChecked(); return !!this._checked; },
    setChecked: function (b)
    {
        this._checked = !!b;
        noteMissing("控件当 CheckBox 用(setChecked)");
    },
    isCheckable: function () { return true; },
    setCheckable: noop,
};

bridgeJz("QLabel", "JZLabel",
    function (textOrParent, parent)
    {
        widgetProtoCtor.call(this);
        if (typeof textOrParent === "string")
            this.setText(textOrParent);
    },
    Object.assign({}, WIDGET_PROTO, {
        setAlignment: function (a) { this.setAlign(a); },
        alignment: function () { return Qt.AlignLeft; },
        clear: function () { this.setText(""); },
        setTextFormat: noop,
        setWordWrap: noop,
        setScaledContents: noop,
        setPixmap: function () { noteMissing("QLabel::setPixmap(框架 JZLabel 只支持文本)"); },
        pixmap: function () { return null; },
        setNum: function (n) { this.setText(String(n)); },
    }));

bridgeJz("QPushButton", "JZPushButton",
    function (textOrParent, parent)
    {
        widgetProtoCtor.call(this);
        if (typeof textOrParent === "string")
            this.setText(textOrParent);
        // 框架的 onClicked(cb) -> 本移植的 sigClicked 数组
        this.onClicked(() => emitSignal(this.sigClicked));
    },
    Object.assign({}, WIDGET_PROTO, {
        // setCheckable/setChecked/isCheckable/isChecked/setIcon/icon 框架已有, 不再覆盖
        click: function () { emitSignal(this.sigClicked); },
        setDefault: noop, setFlat: noop,
    }));

bridgeJz("QLineEdit", "JZLineEdit",
    function (textOrParent, parent)
    {
        widgetProtoCtor.call(this);
        if (typeof textOrParent === "string")
            this.setText(textOrParent);
    },
    Object.assign({}, WIDGET_PROTO, {
        setEchoMode: function (m) { this.setPassword(m === 2); },   // Qt::Password = 2
        echoMode: function () { return this.isPassword() ? 2 : 0; },
        setValidator: function (v) { this._validator = v; },
        validator: function () { return this._validator; },
        setAlignment: noop,
        clear: function () { this.setText(""); },
        selectAll: noop,
        textChanged: function () { },
        // 兄弟协议: 少数 line* 其实是数值框(如 ui.lineLoopTime: setValue/value)
        setValue: function (v) { this._num = v | 0; this.setText(String(v | 0)); noteMissing("控件当 SpinBox 用(setValue)"); },
        value: function () { return (this._num === undefined) ? (Number(this.text()) || 0) : this._num; },
        intValue: function () { return this.value(); },
        setIntValue: function (v) { return this.setValue(v); },
        setRange: function (a, b) { this._min = a | 0; this._max = b | 0; },
        setDecimals: noop, setSingleStep: noop, setSpecialValueText: noop, setPrefix: noop,
        setSuffix: noop, setMaximum: noop, setMinimum: noop, setStep: noop,
    }));

bridgeJz("QCheckBox", "JZCheckbox",
    function (textOrParent, parent)
    {
        widgetProtoCtor.call(this);
        if (typeof textOrParent === "string")
            this.setText(textOrParent);
        // 框架 JZCheckbox 没有"点击"回调: 用基类的鼠标钩子自己合成, 保持信号可用
        this.onMouseRelease = (e) => { emitSignal(this.sigClicked, e); emitSignal(this.sigStateChanged, e); };
    },
    // 兄弟协议: ui.boxDevice / ui.boxType 这些其实当下拉框用(见 SIBLING_COMBO_METHODS)
    Object.assign({}, WIDGET_PROTO, SIBLING_COMBO_METHODS, {
        setTristate: noop,
        checkState: function () { return this.isChecked() ? 2 : 0; },
        setCheckState: function (s) { this.setChecked(s === 2); },
        setEnabled: function (b) { this._enabled = !!b; },
        isEnabled: function () { return this._enabled !== false; },
    }));

bridgeJz("QRadioButton", "JZRadioButton",
    function (textOrParent, parent)
    {
        widgetProtoCtor.call(this);
        if (typeof textOrParent === "string")
            this.setText(textOrParent);
        this.onMouseRelease = (e) => { emitSignal(this.sigClicked, e); emitSignal(this.sigStateChanged, e); };
    },
    Object.assign({}, WIDGET_PROTO, {}));

bridgeJz("QComboBox", "JZComboBox",
    function (parent)
    {
        widgetProtoCtor.call(this);
    },
    Object.assign({}, WIDGET_PROTO, SIBLING_CHECK_METHODS, {
        setCurrentIndex: function (i) { origJz("JZComboBox", "setCurrentIndex").call(this, i | 0); },
        currentIndexChanged: function () { },
        setEditable: function () { },
        setMaxVisibleItems: noop,
    }));

bridgeJz("QSpinBox", "JZSpinBox",
    function (parent)
    {
        widgetProtoCtor.call(this);
    },
    Object.assign({}, WIDGET_PROTO, {
        setSingleStep: function (s) { this.setStep(s); },
        singleStep: function () { return this.step(); },
        setButtonSymbols: noop,
        setAlignment: noop,
        // 框架 JZSpinBox 没有的: 小数位/特殊值文本/前后缀以外的文本接口
        setDecimals: function (d) { this._decimals = d | 0; noteMissing("QSpinBox::setDecimals(框架 JZSpinBox 只有整数)"); },
        decimals: function () { return this._decimals || 0; },
        setSpecialValueText: function (t) { this._specialText = String(t); noteMissing("QSpinBox::setSpecialValueText"); },
        specialValueText: function () { return this._specialText || ""; },
        setPrefix: noop, setWrapping: noop, setAccelerated: noop, setKeyboardTracking: noop,
        text: function () { return String(this.value()); },
        setText: function (t) { const n = parseInt(String(t), 10); if (!isNaN(n)) this.setValue(n); },
    }));

bridgeJz("QSlider", "JZSlider",
    function (orient, parent)
    {
        widgetProtoCtor.call(this);
    },
    Object.assign({}, WIDGET_PROTO, {
        setOrientation: function () { },
        orientation: function () { return Qt.Horizontal; },
        setSingleStep: noop,
    }));

bridgeJz("QProgressBar", "JZProgressBar",
    function (parent)
    {
        widgetProtoCtor.call(this);
    },
    Object.assign({}, WIDGET_PROTO, {
        setFormat: noop,
        setTextVisible: noop,
        setAlignment: noop,
        reset: function () { this.setValue(0); },
    }));

bridgeJz("QGroupBox", "JZGroupBox",
    function (titleOrParent, parent)
    {
        widgetProtoCtor.call(this);
        if (typeof titleOrParent === "string")
            this.setTitle(titleOrParent);
    },
    Object.assign({}, WIDGET_PROTO, {
        setAlignment: noop,
        setCheckable: noop,
        setFlat: noop,
    }));

// ---- 布局: Qt 的 addWidget(多参数形式) / addLayout 框架没有 ----
const LAYOUT_PROTO = {
    // Qt 的 QLayout::setMargin(已废弃但业务在用) = 四边同样的 margin
    setMargin: function (m)
    {
        const f = origJz("JZLayout", "setContentsMargins");
        if (f)
            f.call(this, m | 0, m | 0, m | 0, m | 0);
    },
    setContentsMargins: function (l, t, r, b)
    {
        const f = origJz("JZLayout", "setContentsMargins");
        if (f)
            f.call(this, l | 0, t | 0, r | 0, b | 0);
    },
};
bridgeJz("QGridLayout", "JZGridLayout",
    function (parent) { },
    Object.assign({}, LAYOUT_PROTO, {
        addWidget: function (w, r, c, rs, cs)
        {
            const span = origJz("JZGridLayout", "addWidgetSpan");
            const add = origJz("JZGridLayout", "addWidget");
            const rw = asRealWidget(w);
            if (!rw)
                return;
            if (rs !== undefined && cs !== undefined && span)
                span.call(this, rw, r | 0, c | 0, rs | 0, cs | 0);
            else if (r !== undefined && add)
                add.call(this, rw, r | 0, c | 0);
            else
                noteMissing("QGridLayout::addWidget(widget) 缺行列参数");
        },
        addLayout: function (layout, r, c)
        {
            // 框架布局只接控件: 用一个容器控件带布局来承载
            const holder = new JZWidget();
            if (holder.setLayout)
                holder.setLayout(layout);
            if (r !== undefined)
            {
                const add = origJz("JZGridLayout", "addWidget");
                if (add)
                    add.call(this, holder, r | 0, c | 0);
            }
            else
                noteMissing("QGridLayout::addLayout(无行列参数)");
        },
        // 框架的 JZGridLayout 没有 setSpacing(只有 setHorizontalSpacing/setVerticalSpacing)
        setSpacing: function (s)
        {
            const h = origJz("JZGridLayout", "setHorizontalSpacing");
            const v = origJz("JZGridLayout", "setVerticalSpacing");
            if (h) h.call(this, s | 0);
            if (v) v.call(this, s | 0);
        },
    }));

function boxAddWidget(jzName, w, stretch)
{
    const add = origJz(jzName, "addWidget");
    const rw = asRealWidget(w);
    if (!add || !rw)
        return;
    add.call(this, rw, stretch === undefined ? 0 : stretch);
}
const BOX_LAYOUT_PROTO = {
    addWidget: function (w, stretch)
    {
        return boxAddWidget.call(this, this.__jzName, w, stretch);
    },
    addLayout: function (layout, stretch)
    {
        const holder = new JZWidget();
        if (holder.setLayout)
            holder.setLayout(layout);
        return boxAddWidget.call(this, this.__jzName, holder, stretch);
    },
};
bridgeJz("QVBoxLayout", "JZVBoxLayout",
    function (parent) { this.__jzName = "JZVBoxLayout"; },
    BOX_LAYOUT_PROTO);
bridgeJz("QHBoxLayout", "JZHBoxLayout",
    function (parent) { this.__jzName = "JZHBoxLayout"; },
    BOX_LAYOUT_PROTO);

// Qt 具名颜色要在桥之后重建, 让 Qt.red 之类也是框架色
for (const n of ["white", "black", "red", "darkRed", "green", "darkGreen", "blue", "darkBlue",
    "cyan", "darkCyan", "magenta", "darkMagenta", "yellow", "darkYellow", "gray", "darkGray",
    "lightGray", "orange"])
    Qt[n] = new g_globals.QColor(n);

// ---- 画笔: QPainter -> 框架 JZPainter ----
//   框架的 JZPainter 构造必须带设备(widget = 屏幕画笔, 只能 onPaint 内用; image = 离屏),
//   所以这里不用 bridgeJz 的"无参 new + 补参数", 而是**转发构造参数**的工厂函数。
//   Qt 的重载(QRect/QPoint 版本、setPen(QPen)/setBrush(QBrush)/setFont(QFont))在原型上补齐。
function cssOf(colorLike)
{
    if (colorLike === undefined || colorLike === null)
        return "#000000";
    if (typeof colorLike === "string")
        return colorLike;
    if (typeof colorLike.css === "function")
        return colorLike.css();
    if (typeof colorLike.name === "function")
        return colorLike.name();
    return String(colorLike);
}

if (typeof JZPainter === "function")
{
    const P = JZPainter.prototype;
    const orig = {};
    const pn = Object.getOwnPropertyNames(P);
    for (let i = 0; i < pn.length; i++)
        orig[pn[i]] = P[pn[i]];

    function callOrig(name, self, args)
    {
        if (typeof orig[name] !== "function")
        {
            noteMissing("JZPainter." + name);
            return undefined;
        }
        return orig[name].apply(self, args);
    }

    P.setPen = function (a, b)
    {
        if (a !== null && typeof a === "object" && typeof a.color === "function")
            return callOrig("setPen", this, [cssOf(a.color()), (typeof a.width === "function") ? a.width() : 1]);
        if (typeof a === "number")
            return noteMissing("QPainter::setPen(风格枚举, 框架只支持颜色+线宽)");
        return callOrig("setPen", this, [cssOf(a), (b === undefined) ? 1 : b]);
    };
    P.setBrush = function (a)
    {
        if (a !== null && typeof a === "object" && typeof a.color === "function")
            return callOrig("setBrush", this, [cssOf(a.color())]);
        return callOrig("setBrush", this, [cssOf(a)]);
    };
    P.setFont = function (a, b)
    {
        if (a !== null && typeof a === "object" && typeof a.family === "function")
            return callOrig("setFont", this, [a.family(), (typeof a.pointSize === "function") ? a.pointSize() : 12]);
        return callOrig("setFont", this, [String(a), (b === undefined) ? 12 : b]);
    };
    P.drawRect = function (a, b, c, d)
    {
        if (a !== null && typeof a === "object" && typeof a.width === "function")
            return callOrig("drawRect", this, [a.x(), a.y(), a.width(), a.height()]);
        return callOrig("drawRect", this, [a, b, c, d]);
    };
    P.fillRect = function (a, b, c, d, e)
    {
        if (a !== null && typeof a === "object" && typeof a.width === "function")
        {   // fillRect(QRect, QColor|QBrush)
            const col = (b !== null && typeof b === "object" && typeof b.color === "function") ? b.color() : b;
            return callOrig("fillRect", this, [a.x(), a.y(), a.width(), a.height(), cssOf(col)]);
        }
        return callOrig("fillRect", this, [a, b, c, d, cssOf(e)]);
    };
    P.drawLine = function (a, b, c, d)
    {
        if (a !== null && typeof a === "object" && typeof a.x === "function")
        {   // drawLine(QPoint, QPoint)
            const p2 = b || a;
            return callOrig("drawLine", this, [a.x(), a.y(), p2.x(), p2.y()]);
        }
        return callOrig("drawLine", this, [a, b, c, d]);
    };
    P.drawText = function (a, b, c, d)
    {
        let x = a, y = b, text = c;
        if (a !== null && typeof a === "object" && typeof a.x === "function")
        {   // drawText(QPoint, text)
            x = a.x(); y = a.y(); text = b;
        }
        else if (a !== null && typeof a === "object" && typeof a.width === "function")
        {   // drawText(QRect, flags, text)
            x = a.x(); y = a.y() + ((d === undefined) ? 0 : 0); text = (typeof c === "string") ? c : d;
            y = a.y();
        }
        else if (typeof text !== "string" && typeof c !== "string")
        {
            text = (c === undefined) ? b : c;      // drawText(x, y, text)
        }
        return callOrig("drawText", this, [x, y, String(text === undefined ? "" : text)]);
    };
    P.setClipRect = function (a, b, c, d)
    {
        if (a !== null && typeof a === "object" && typeof a.width === "function")
            return callOrig("setClip", this, [a.x(), a.y(), a.width(), a.height()]);
        return callOrig("setClip", this, [a, b, c, d]);
    };
    P.setRenderHint = noop;
    P.setRenderHints = noop;
    P.setOpacity = noop;
    P.setCompositionMode = noop;
    P.setWindow = noop;
    P.setViewport = noop;
    P.setWorldMatrixEnabled = noop;
    P.setMatrix = noop;
    P.resetMatrix = noop;
    P.pen = function () { return null; };
    P.brush = function () { return null; };

    function QPainter(device) { return new JZPainter(device); }
    QPainter.prototype = JZPainter.prototype;
    Object.setPrototypeOf(QPainter, JZPainter);
    QPainter.jzBase = "JZPainter";
    g_globals.QPainter = new Proxy(QPainter, {
        apply(target, thisArg, args)
        {
            if (thisArg !== undefined && thisArg !== null && typeof thisArg === "object")
            {
                attachQtFallback(thisArg, "JZPainter");    // QPainter::xxx(this) 之类
                return undefined;
            }
            return target(...args);
        },
    });
    frameworkBridge.real.push("QPainter <- JZPainter(真绘制: 屏幕/离屏画笔)");
}

// QThread 走框架真 worker(独立 runtime + 线程 + 消息, 见上面 QThread 的说明)
if (typeof globalThis.Worker === "function")
    frameworkBridge.real.push("QThread <- Worker(worker 模块: 独立 runtime + 消息通信)");

// ---------------------------------------------------------------------------
// 视图类(引擎已注册: JZTableWidget / JZListWidget / JZTabWidget / JZStackedWidget /
//   JZSplitter / JZScrollArea / JZScrollBar / JZTextEdit / JZImageLabel)
//
//   框架的表/列表是**值级**接口(setItemText/itemText/appendItem/...): 没有 Qt 那种
//   "单元格对象"。所以 QTableWidgetItem/QListWidgetItem 在 JS 侧补, 并且:
//     * 表格项**写穿** —— item.setText() 直接落到真单元格(table.setItemText);
//     * item 上带 data(role) —— 移植代码用 Qt.UserRole 在列表项上挂业务数据;
//     * item 的样式(setForeground/setBackground/setIcon/对齐)框架没有 → 记账 + 存 JS 侧。
//   这样界面上的表/列表就是**真控件 + 真数据**, 只有"单元格装饰"这层是 JS。
// ---------------------------------------------------------------------------
class QTableWidgetItemJS
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor(text)
    {
        this._text = (text === undefined || text === null) ? "" : String(text);
        this._data = new Map();
    }
    text() { return this._text; }
    setText(t) { this._text = (t === undefined || t === null) ? "" : String(t); return this._sync(); }
    _attach(table, row, col) { this._table = table; this._row = row | 0; this._col = col | 0; return this._sync(); }
    _sync()
    {
        if (this._table && typeof this._table.setItemText === "function")
            this._table.setItemText(this._row, this._col, this._text);
        return this;
    }
    // Qt 的 setData/data(role): 业务数据挂这里(框架的单元格只存文本)
    data(role) { return this._data.get(role | 0); }
    setData(role, v) { this._data.set(role | 0, v); return this; }
    // 单元格装饰: 框架表格没有 → 记账一次, 值留在 JS 侧(不影响文本/布局)
    setForeground(c) { this._fg = c; noteMissing("QTableWidgetItem::setForeground"); return this; }
    setBackground(c) { this._bg = c; noteMissing("QTableWidgetItem::setBackground"); return this; }
    foreground() { return this._fg || null; }
    background() { return this._bg || null; }
    setIcon(i) { this._icon = i; noteMissing("QTableWidgetItem::setIcon"); return this; }
    icon() { return this._icon || null; }
    setTextAlignment(a) { this._align = a | 0; return this; }
    textAlignment() { return this._align || 0; }
    setToolTip(t) { this._tip = t; return this; }
    toolTip() { return this._tip || ""; }
    setFlags(f) { this._flags = f; return this; }
    flags() { return (this._flags === undefined) ? 0 : this._flags; }
    setCheckState(s) { this._check = s; return this; }
    checkState() { return this._check || 0; }
}

class QListWidgetItemJS
{
    constructor(text)
    {
        this._text = (text === undefined || text === null) ? "" : String(text);
        this._data = new Map();
    }
    text() { return this._text; }
    setText(t)
    {
        this._text = (t === undefined || t === null) ? "" : String(t);
        return this;
    }
    data(role) { return this._data.get(role | 0); }
    setData(role, v) { this._data.set(role | 0, v); return this; }
    setIcon(i) { this._icon = i; noteMissing("QListWidgetItem::setIcon"); return this; }
    icon() { return this._icon || null; }
}

// 表头(QHeaderView)的链式调用: 框架表格没有表头对象 → 宽容对象接住, 统一记一笔账
function headerStub(what)
{
    noteMissing(what);
    return makeLenient(what, true);
}

// 单元格/列表项对象: 框架没有对应绑定, 用上面两个 JS 类(覆盖 GUI_STUB_CLASSES 里的空壳)
g_globals.QTableWidgetItem = QTableWidgetItemJS;
g_globals.QListWidgetItem = QListWidgetItemJS;

bridgeJz("QTableWidget", "JZTableWidget",
    function (rows, cols, parent)
    {
        widgetProtoCtor.call(this);
        this._items = new Map();
        if (rows)
            this.setRowCount(rows | 0);
        if (cols)
            this.setColumnCount(cols | 0);
    },
    Object.assign({}, WIDGET_PROTO, {
        // Qt 的 item(r,c) —— 框架没有单元格对象, 这里给 JS 侧对象(首次从真单元格取文本)
        // 注: _items 是**桥的构造里**建的; 但迁移期表格可能是真 UI(uiLoader)直接 new 的 JZTableWidget
        //     —— 那种实例没走桥的构造, 所以这里(以及 setItem/clearContents)必须能自愈。
        item: function (r, c)
        {
            if (!this._items)
                this._items = new Map();
            const key = (r | 0) + "," + (c | 0);
            let it = this._items.get(key);
            if (!it)
            {
                const f = origJz("JZTableWidget", "itemText");
                it = new QTableWidgetItemJS(f ? f.call(this, r | 0, c | 0) : "");
                it._attach(this, r, c);
                this._items.set(key, it);
            }
            return it;
        },
        // Qt 的 setItem(r,c,item) —— 写穿到真单元格
        setItem: function (r, c, item)
        {
            if (!this._items)
                this._items = new Map();
            const key = (r | 0) + "," + (c | 0);
            if (!item)
            {
                this._items.delete(key);
                return;
            }
            item._attach(this, r, c);
            this._items.set(key, item);
        },
        clearContents: function ()
        {
            if (this._items)
                this._items.clear();
            const f = origJz("JZTableWidget", "clearContents");
            if (f) f.call(this);
        },
        setRowCount: function (n)
        {
            const f = origJz("JZTableWidget", "setRowCount");
            if (f) f.call(this, n | 0);
            if (this._items) this._prune();
        },
        setColumnCount: function (n)
        {
            const f = origJz("JZTableWidget", "setColumnCount");
            if (f) f.call(this, n | 0);
            if (this._items) this._prune();
        },
        // 行列数变小后清掉越界的单元格缓存(否则 item() 会回旧值)
        _prune: function ()
        {
            const rc = (typeof this.rowCount === "function") ? this.rowCount() : 0;
            const cc = (typeof this.columnCount === "function") ? this.columnCount() : 0;
            const keys = Array.from(this._items.keys());
            for (let i = 0; i < keys.length; i++)
            {
                const p = keys[i].split(",");
                if ((p[0] | 0) >= rc || (p[1] | 0) >= cc)
                    this._items.delete(keys[i]);
            }
        },
        // 框架表格没有的能力: 统一 no-op/记账(界面仍可用, 只是少了这层特性)
        setCellWidget: function () { noteMissing("QTableWidget::setCellWidget(框架表格无单元格控件)"); },
        cellWidget: function () { noteMissing("QTableWidget::cellWidget(框架表格无单元格控件)"); return null; },
        setCurrentItem: function (item)
        {
            // Qt 按 item 设当前单元格: 从缓存里反查行列
            const keys = Array.from(this._items.keys());
            for (let i = 0; i < keys.length; i++)
            {
                if (this._items.get(keys[i]) === item)
                {
                    const p = keys[i].split(",");
                    const f = origJz("JZTableWidget", "setCurrentCell");
                    if (f) f.call(this, p[0] | 0, p[1] | 0);
                    return;
                }
            }
        },
        itemAt: function () { noteMissing("QTableWidget::itemAt(框架表格无命中测试)"); return null; },
        horizontalHeader: function () { return headerStub("QTableWidget::horizontalHeader"); },
        verticalHeader: function () { return headerStub("QTableWidget::verticalHeader"); },
        setSelectionMode: noop, setSelectionBehavior: noop, setEditTriggers: noop,
        setSortingEnabled: noop, setAlternatingRowColors: noop, setShowGrid: noop,
        setWordWrap: noop, setCornerButtonEnabled: noop, resizeColumnsToContents: noop,
        resizeRowsToContents: noop, scrollToItem: noop, setColumnHidden: noop,
    }));
// Qt 的静态枚举(QTableWidget::NoEditTriggers 之类; 值与 QAbstractItemView 对齐)
{
    const T = g_globals.QTableWidget;
    T.NoEditTriggers = 0; T.DoubleClicked = 2; T.SelectedClicked = 4; T.CurrentChanged = 0;
    T.NoSelection = 0; T.SingleSelection = 1; T.MultiSelection = 2; T.ExtendedSelection = 3;
    T.SelectItems = 0; T.SelectRows = 1; T.SelectColumns = 2;
}

bridgeJz("QListWidget", "JZListWidget",
    function (parent)
    {
        widgetProtoCtor.call(this);
        this._items = [];         // 按加入顺序存 JS 侧项对象(带 data(role))
    },
    Object.assign({}, WIDGET_PROTO, {
        addItem: function (item)
        {
            const text = (item && typeof item.text === "function") ? item.text() : String(item === undefined ? "" : item);
            const f = origJz("JZListWidget", "appendItem");
            if (f) f.call(this, text);
            if (item && typeof item === "object")
                this._items.push(item);
        },
        addItems: function (arr)
        {
            const f = origJz("JZListWidget", "appendItems");
            if (f && arr) f.call(this, arr);
        },
        item: function (i)
        {
            const idx = i | 0;
            if (this._items && this._items[idx])
                return this._items[idx];
            const f = origJz("JZListWidget", "itemText");
            const it = new QListWidgetItemJS(f ? f.call(this, idx) : "");
            if (this._items) this._items[idx] = it;
            return it;
        },
        itemText: function (i) { const f = origJz("JZListWidget", "itemText"); return f ? f.call(this, i | 0) : ""; },
        count: function () { const f = origJz("JZListWidget", "count"); return f ? f.call(this) : 0; },
        clear: function () { this._items = []; const f = origJz("JZListWidget", "clear"); if (f) f.call(this); },
        currentRow: function () { const f = origJz("JZListWidget", "currentIndex"); return f ? f.call(this) : -1; },
        setCurrentRow: function (i) { const f = origJz("JZListWidget", "setCurrentIndex"); if (f) f.call(this, i | 0); },
        currentItem: function () { const i = this.currentRow(); return (i < 0) ? null : this.item(i); },
        setSelectionMode: noop, setSelectionBehavior: noop, setEditTriggers: noop,
        setSortingEnabled: noop, setWordWrap: noop, setIconSize: noop, setSpacing: noop,
    }));

bridgeJz("QTabWidget", "JZTabWidget",
    function (parent) { widgetProtoCtor.call(this); this._pages = []; },
    Object.assign({}, WIDGET_PROTO, {
        addTab: function (page, label)
        {
            const f = origJz("JZTabWidget", "addTab");
            const idx = f ? f.call(this, page, String(label === undefined ? "" : label)) : -1;
            if (this._pages) this._pages.push(page);
            return idx;
        },
        count: function () { const f = origJz("JZTabWidget", "count"); return f ? f.call(this) : 0; },
        tabText: function (i) { const f = origJz("JZTabWidget", "tabText"); return f ? f.call(this, i | 0) : ""; },
        currentIndex: function () { const f = origJz("JZTabWidget", "currentIndex"); return f ? f.call(this) : -1; },
        setCurrentIndex: function (i) { const f = origJz("JZTabWidget", "setCurrentIndex"); if (f) f.call(this, i | 0); },
        // Qt 的按控件切页(框架只有按序号): 用 addTab 顺序反查
        widget: function (i) { return (this._pages || [])[i | 0] || null; },
        indexOf: function (w) { return (this._pages || []).indexOf(w); },
        setCurrentWidget: function (w)
        {
            const i = (this._pages || []).indexOf(w);
            if (i >= 0) this.setCurrentIndex(i);
            else noteMissing("QTabWidget::setCurrentWidget(该控件不是本标签页的子页)");
        },
        setTabText: noop, removeTab: noop, setTabsClosable: noop, setMovable: noop, setDocumentMode: noop,
    }));

bridgeJz("QStackedWidget", "JZStackedWidget",
    function (parent) { widgetProtoCtor.call(this); this._pages = []; },
    Object.assign({}, WIDGET_PROTO, {
        addWidget: function (w)
        {
            const f = origJz("JZStackedWidget", "addWidget");
            if (f) f.call(this, w);
            if (this._pages) this._pages.push(w);
        },
        count: function () { const f = origJz("JZStackedWidget", "count"); return f ? f.call(this) : 0; },
        currentIndex: function () { const f = origJz("JZStackedWidget", "currentIndex"); return f ? f.call(this) : -1; },
        setCurrentIndex: function (i) { const f = origJz("JZStackedWidget", "setCurrentIndex"); if (f) f.call(this, i | 0); },
        currentWidget: function () { const f = origJz("JZStackedWidget", "currentWidget"); return f ? f.call(this) : null; },
        // Qt 的按控件切页: ui.stackedWidget.setCurrentWidget(ui.pageNormal) —— 移植里很常用
        widget: function (i) { return (this._pages || [])[i | 0] || null; },
        indexOf: function (w) { return (this._pages || []).indexOf(w); },
        setCurrentWidget: function (w)
        {
            const i = (this._pages || []).indexOf(w);
            if (i >= 0) this.setCurrentIndex(i);
            else noteMissing("QStackedWidget::setCurrentWidget(该控件不是本堆叠页的子页)");
        },
        insertWidget: function (i, w) { this.addWidget(w); },   // 框架只有 addWidget(追加)
    }));

bridgeJz("QSplitter", "JZSplitter",
    function (orientOrParent, parent) { widgetProtoCtor.call(this); },
    Object.assign({}, WIDGET_PROTO, {
        addWidget: function (w)
        {
            const f = origJz("JZSplitter", "addWidget");
            if (f) f.call(this, w);
        },
        insertWidget: function (i, w)
        {
            const f = origJz("JZSplitter", "insertWidget");
            if (f) f.call(this, i | 0, w);
        },
        setOrientation: function (o) { const f = origJz("JZSplitter", "setOrientation"); if (f) f.call(this, o | 0); },
        orientation: function () { const f = origJz("JZSplitter", "orientation"); return f ? f.call(this) : 0; },
        setHandleWidth: function (w) { const f = origJz("JZSplitter", "setHandleWidth"); if (f) f.call(this, w | 0); },
        setStretchFactor: function (i, v) { const f = origJz("JZSplitter", "setStretchFactor"); if (f) f.call(this, i | 0, v | 0); },
        setCollapsible: function (i, c) { const f = origJz("JZSplitter", "setCollapsible"); if (f) f.call(this, i | 0, !!c); },
        count: function () { const f = origJz("JZSplitter", "count"); return f ? f.call(this) : 0; },
        removeWidget: function (w) { const f = origJz("JZSplitter", "removeWidget"); if (f) f.call(this, w); },
        setSizes: noop, sizes: function () { return []; }, saveState: function () { return null; },
        restoreState: function () { return false; },
        // Qt 的 setStretchFactor 与 addWidget(w, stretch) 在框架里分别对应
        // setStretchFactor(i, v) / 只 addWidget —— addWidget 的第二参忽略
    }));

bridgeJz("QTextEdit", "JZTextEdit",
    function (textOrParent, parent)
    {
        widgetProtoCtor.call(this);
        if (typeof textOrParent === "string")
            this.setText(textOrParent);
    },
    Object.assign({}, WIDGET_PROTO, {
        setPlainText: function (s) { this.setText(String(s)); },
        toPlainText: function () { return this.text(); },
        append: function (s)
        {
            const cur = this.text();
            this.setText(cur + (cur.length ? "\n" : "") + String(s));
            if (typeof this.setCursorPosition === "function")
                this.setCursorPosition(this.text().length);
        },
        insertPlainText: function (s) { this.append(s); },
        setReadOnly: function (b) { const f = origJz("JZTextEdit", "setReadOnly"); if (f) f.call(this, !!b); },
        isReadOnly: function () { const f = origJz("JZTextEdit", "isReadOnly"); return f ? !!f.call(this) : false; },
        setPlaceholderText: function (s) { const f = origJz("JZTextEdit", "setPlaceholderText"); if (f) f.call(this, String(s)); },
        setCursorPosition: function (p) { const f = origJz("JZTextEdit", "setCursorPosition"); if (f) f.call(this, p | 0); },
        setLineWrapMode: noop, setWordWrapMode: noop, setAcceptRichText: noop,
        setVerticalScrollBarPolicy: noop, setHorizontalScrollBarPolicy: noop, setFrameShape: noop,
        setUndoRedoEnabled: noop, setTabStopDistance: noop, setFontPointSize: noop, setHtml: noop,
        toHtml: function () { return this.text(); },
        clear: function () { this.setText(""); },
        // QTextBrowser 专有(移植里 ui.textHelp 用): 记账即可
        setOpenExternalLinks: function () { noteMissing("QTextBrowser::setOpenExternalLinks"); },
        setOpenLinks: function () { noteMissing("QTextBrowser::setOpenLinks"); },
        setTextInteractionFlags: noop, setAnchorClicked: noop,
    }));

bridgeJz("QScrollArea", "JZScrollArea",
    function (parent) { widgetProtoCtor.call(this); },
    Object.assign({}, WIDGET_PROTO, {
        setWidget: function (w)
        {
            const f = origJz("JZScrollArea", "setWidget");
            if (f) f.call(this, w);
        },
        setWidgetResizable: noop, setAlignment: noop, setFrameShape: noop,
        setVerticalScrollBarPolicy: noop, setHorizontalScrollBarPolicy: noop,
    }));

bridgeJz("QScrollBar", "JZScrollBar",
    function (orientOrParent, parent) { widgetProtoCtor.call(this); },
    Object.assign({}, WIDGET_PROTO, {}));

// QImage / QPixmap -> 框架 JZImage(注意: 框架没有"从文件加载"的绑定, 只有 create(w,h))
{
    function imageFactory(what)
    {
        function QImage(a, b)
        {
            if (typeof a === "string")
            {
                noteMissing(what + "(文件路径)(框架 JZImage 没有文件构造)");
                return new JZImage();
            }
            return JZImage.create(a | 0, b | 0);
        }
        QImage.prototype = JZImage.prototype;
        Object.setPrototypeOf(QImage, JZImage);
        QImage.jzBase = "JZImage";
        return QImage;
    }
    g_globals.QImage = imageFactory("QImage");
    g_globals.QPixmap = imageFactory("QPixmap");
    frameworkBridge.real.push("QImage/QPixmap <- JZImage(内存图; 文件加载未绑定)");
}

// ---- Ui_*Class: 成员按"名字约定"造**真控件** ----
//
// 仓库里没有 .ui 文件 → 没有 uic 生成的界面代码。但移植代码里的 `this.ui.tableQuery`
// 这些名字就是 .ui 的对象名, 命名很规律(table*/line*/box*/btn*/label*/spin*/text*/...):
// 这里按约定造真控件(QTableWidget/QLineEdit/QCheckBox/...), 认不出的继续用宽容空壳。
// 于是 `this.ui.tableQuery.setItem(...)` 落到的是**真表格** —— 界面布局仍等 .ui, 但控件
// 与数据链路已经是真的(首个访问时创建并缓存, 不会白造一堆)。
// setupUi 只记账: 没有 .ui 的布局信息, 不做摆放。
function uiMemberFactory(name)
{
    const make = (n) =>
    {
        const C = g_globals[n];
        if (typeof C !== "function")
            return undefined;
        try { return new C(); } catch (e) { return undefined; }
    };
    // 无绑定的(树/第三方图表) -> 保持宽容空壳
    if (/^(tree|plot|calendar)/i.test(name))
        return undefined;
    // 日期控件: JS 侧真实现(内部是 Date), 如 ui.dateFrom / ui.dateTo
    if (/^(date|timeEdit|datetime)/i.test(name)) return make("QDateTimeEdit");
    if (/^table/i.test(name)) return make("QTableWidget");
    if (/^(listWidget|list)$/i.test(name)) return make("QListWidget");
    if (/^(tabWidget|tabs)/i.test(name)) return make("QTabWidget");
    if (/^stacked/i.test(name)) return make("QStackedWidget");
    if (/^(spin|sb[A-Z])/.test(name)) return make("QSpinBox");
    if (/^(box|check)/i.test(name)) return make("QCheckBox");
    if (/^line/i.test(name)) return make("QLineEdit");
    if (/^(text|log|memo)/i.test(name)) return make("QTextEdit");
    if (/^(label|lab[A-Z0-9])/.test(name)) return make("QLabel");
    if (/^(btn|button|push)/i.test(name)) return make("QPushButton");
    if (/^combo/i.test(name)) return make("QComboBox");
    if (/^slider/i.test(name)) return make("QSlider");
    if (/^progress/i.test(name)) return make("QProgressBar");
    if (/^group/i.test(name)) return make("QGroupBox");
    if (/^scrollArea/i.test(name)) return make("QScrollArea");
    if (/^splitter/i.test(name)) return make("QSplitter");
    if (/^action/i.test(name)) return make("QAction");
    if (/^(page|widget|container|itemWidgets|central|content|panel|frame)/i.test(name))
        return make("QWidget");
    return undefined;      // 认不出 -> 宽容空壳(记账)
}
for (const name of UI_STUB_CLASSES)
    g_globals[name] = makeQtStubClass(name, {}, uiMemberFactory);
frameworkBridge.real.push("Ui_*Class 成员按名字约定造真控件(无 .ui 布局信息)");

// 没走桥的 Qt 名(仍用本文件的 JS 实现)
for (const n of ["QFile", "QDir", "QTextStream", "QSettings", "QPen", "QBrush", "QFont", "QFontMetrics",
    "QObject", "QApplication", "QMessageBox", "QTableWidgetItem", "QListWidgetItem",
    "QTreeWidget", "QTreeWidgetItem", "QMenu", "QAction", "QActionGroup", "QShortcut", "QKeySequence",
    "QLinearGradient", "QTextOption"])
    frameworkBridge.js.push(n);

if (typeof globalThis.JZPainter !== "function")
    frameworkBridge.js.push("QPainter(引擎没注册 JZPainter)");

if (typeof globalThis.Worker !== "function")
    frameworkBridge.js.push("QThread(引擎没装 worker 模块)");

// 挂到兼容层本体上(业务/测试都通过 __bte.qtCompat 访问)
QtCompat.bridge = frameworkBridge;          // Qt 名 -> 框架真实现 的映射清单
QtCompat.bridgeReport = frameworkBridgeReport;

export function frameworkBridgeReport()
{
    console.log("[qtcompat] 框架桥: " + frameworkBridge.real.length + " 项用框架真实现, "
        + frameworkBridge.js.length + " 项仍是 JS 定义");
    for (const r of frameworkBridge.real)
        console.log("  [JZ] " + r);
    console.log("  [JS] " + frameworkBridge.js.join(", "));
    if (g_realWidgetClasses.length)
        console.log("  [真控件化] " + g_realWidgetClasses.join(", "));
    return frameworkBridge;
}

// 安装到 globalThis(业务文件里都是裸名字引用)
for (const k in g_globals)
{
    try
    {
        globalThis[k] = g_globals[k];
    }
    catch (e)
    {
        console.error("[qtcompat] 无法注入全局 " + k + ": " + e);
    }
}

// 导出(供测试与显式 import)
export {
    Qt, QtMsgType, QtDebugMsg, QtInfoMsg, QtWarningMsg, QtCriticalMsg, QtFatalMsg,
    NULL, TRUE, FALSE, SND_FILENAME, SND_LOOP, SND_ASYNC, SND_NODEFAULT, EXCEPTION_ACCESS_VIOLATION,
    QColor, QPoint, QSize, QRect, QPen, QBrush, QFont, QFontMetrics, QLinearGradient, QTextOption,
    QIODevice,
    QFile, QDir, QTextStream, QSettings,
    QObject, QApplication, QTranslator, QSharedMemory, QThread, QAtomicInteger,
    QMessageBox, QFileDialog, QInputDialog,
    qRgb, qRgba, makeLenient, makeQtStubClass, QtCompat, report as qtCompatReport,
};
export default QtCompat;
