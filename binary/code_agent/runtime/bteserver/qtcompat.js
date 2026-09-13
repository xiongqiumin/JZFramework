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

// QApplication::applicationDirPath() 的取值: 默认取可执行文件目录, 可被显式覆盖。
let g_appDir = null;
function appDirPath()
{
    if (g_appDir !== null)
        return g_appDir;
    const c = jzCore();
    return c ? dirName(c.execPath()) : ".";
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
const Qt = {
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
const QtDebugMsg = 0, QtInfoMsg = 4, QtWarningMsg = 1, QtCriticalMsg = 2, QtFatalMsg = 3;
const QtMsgType = { QtDebugMsg, QtInfoMsg, QtWarningMsg, QtCriticalMsg, QtFatalMsg };

// Win32 常量(AlarmPlayer / main.js 里按原样保留的调用)
const NULL = null;
const TRUE = 1, FALSE = 0;
const SND_SYNC = 0x0000, SND_ASYNC = 0x0001, SND_NODEFAULT = 0x0002,
    SND_MEMORY = 0x0004, SND_LOOP = 0x0008, SND_FILENAME = 0x00020000;
const EXCEPTION_ACCESS_VIOLATION = 0xc0000005;

// ---------------------------------------------------------------------------
// 4. 值类型
// ---------------------------------------------------------------------------
class QColor
{
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
function qRgb(r, g, b) { return (0xff000000 | (r << 16) | (g << 8) | b) >>> 0; }
function qRgba(r, g, b, a) { return ((a << 24) | (r << 16) | (g << 8) | b) >>> 0; }

class QPoint
{
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
    constructor(w, h) { this._w = w | 0; this._h = h | 0; }
    width() { return this._w; }
    height() { return this._h; }
    setWidth(v) { this._w = v | 0; }
    setHeight(v) { this._h = v | 0; }
    isEmpty() { return this._w <= 0 || this._h <= 0; }
}

class QRect
{
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

class QPen
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

class QBrush
{
    constructor(c) { this._color = (c instanceof QColor || typeof c === "number") ? new QColor(c) : new QColor(0, 0, 0); }
    color() { return this._color; }
    setColor(c) { this._color = (c instanceof QColor) ? c : new QColor(c); }
    style() { return 1; }
}

class QFont
{
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

class QFontMetrics
{
    constructor(font) { this._font = font; }
    horizontalAdvance(t) { return String(t).length * ((this._font && this._font.pixelSize()) || 12); }
    width(t) { return this.horizontalAdvance(t); }
    height() { return ((this._font && this._font.pixelSize()) || 12) + 2; }
    boundingRect(t) { return new QRect(0, 0, this.horizontalAdvance(t), this.height()); }
}

class QLinearGradient
{
    constructor() { this._stops = []; this._c0 = new QPoint(0, 0); this._c1 = new QPoint(0, 1); }
    setStart(x, y) { this._c0 = new QPoint(x, y); }
    setFinalStop(x, y) { this._c1 = new QPoint(x, y); }
    setColorAt(pos, color) { this._stops.push({ pos, color }); }
}

class QTextOption
{
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

class QFile
{
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

class QDir
{
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

class QTextStream
{
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

class QSettings
{
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

class QApplication extends QObject
{
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
    quit() { noteMissing("QApplication::quit"); }
    exit(code) { noteMissing("QApplication::exit"); }
    processEvents() { }
    translate(context, text) { return text; }
    installTranslator(t) { this._translator = t; }
    removeTranslator(t) { if (this._translator === t) this._translator = null; }
    arguments() { return this._argv; }
    keyboardModifiers() { return Qt.NoModifier; }
    setStyleSheet() { noteMissing("QApplication::setStyleSheet"); }
    static applicationDirPath() { return appDirPath(); }
    static setApplicationDirPath(p) { g_appDir = String(p); }
    static applicationFilePath() { const c = jzCore(); return c ? c.execPath() : ""; }
    static translate(context, text) { return text; }
    static instance() { return QApplication._inst; }
    static primaryScreen() { return makeLenient("QScreen"); }
    static setOverrideCursor() { noteMissing("QApplication::setOverrideCursor"); }
    static restoreOverrideCursor() { }
    static closeAllWindows() { }
    static get KeypadModifier() { return Qt.KeypadModifier; }
    static get ControlModifier() { return Qt.ControlModifier; }
    static get ShiftModifier() { return Qt.ShiftModifier; }
}
QApplication._inst = null;

class QTranslator extends QObject
{
    constructor(parent) { super(parent); this._loaded = false; }
    load(a, b) { this._loaded = true; noteMissing("QTranslator::load(.qm 未接入, 直接当作加载成功)"); return true; }
    isEmpty() { return !this._loaded; }
}

class QSharedMemory extends QObject
{
    constructor(key, parent) { super(parent); this._key = key; this._attached = false; }
    attach() { return false; }
    detach() { this._attached = false; }
    create(size) { this._attached = true; return true; }
    isAttached() { return this._attached; }
    lock() { return true; }
    unlock() { return true; }
    data() { return null; }
}

class QThread extends QObject
{
    constructor(parent) { super(parent); this.sigStarted = []; this.sigFinished = []; this.sigResult = []; }
    start() { noteMissing("QThread::start(JS 引擎无线程, 见 ServerThread 的删除)"); for (const f of this.sigStarted) f(); }
    quit() { }
    wait() { return true; }
    isRunning() { return false; }
}

class QAtomicInteger
{
    constructor(v) { this._v = v | 0; }
    load() { return this._v; }
    store(v) { this._v = v | 0; }
    fetchAndAdd(v) { const o = this._v; this._v += v; return o; }
    operator_plus_plus() { return this._v++; }
}

// ---------------------------------------------------------------------------
// 10. 消息框 / 对话框(无 GUI 时: 确定性默认返回值 + 记账, 绝不弹窗阻塞)
// ---------------------------------------------------------------------------
class QMessageBox extends QObject
{
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

class QFileDialog extends QObject
{
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

class QInputDialog extends QObject
{
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
function makeLenient(name)
{
    noteMissing(name + " (空壳实例)");
    const store = Object.create(null);
    const target = { __qtstub: name };
    const handler = {
        get(t, prop)
        {
            if (prop === "__qtstub") return name;
            if (prop === Symbol.toPrimitive) return () => 0;
            if (prop === "toString") return () => "[" + name + "]";
            if (prop === "valueOf") return () => 0;
            if (prop === "toJSON") return () => null;
            if (prop in store) return store[prop];
            if (typeof prop === "string" && prop.slice(0, 3) === "sig")
            {
                store[prop] = [];
                return store[prop];
            }
            const fn = function () { return makeLenient(name + "." + String(prop)); };
            store[prop] = fn;
            return fn;
        },
        set(t, prop, v) { store[prop] = v; return true; },
        has() { return true; },
        deleteProperty(t, prop) { delete store[prop]; return true; },
        ownKeys() { return Object.keys(store); },
        getOwnPropertyDescriptor(t, prop) { return { configurable: true, enumerable: true, value: store[prop] }; },
    };
    return new Proxy(target, handler);
}

// GUI 类: 构造/静态访问都宽容; 支持 `QWidget.call(this, parent)` 这种"以函数形式初始化"
//   成员当作选项传入
function makeQtStubClass(name, statics)
{
    const S = Object.assign({}, statics || {});
    const tokCache = Object.create(null);

    function ctor()
    {
        // 只有 QWidget.call(this, parent) 这类显式调用会走到这里
        noteMissing(name + " (空壳基类初始化)");
        if (this && typeof this === "object")
            this.__qt = name;
        return undefined;
    }

    const proxy = new Proxy(ctor, {
        construct() { return makeLenient(name); },
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

// GUI 空壳类清单: 引擎里没有的 Qt 控件/绘图/布局, 一律宽容空壳
const GUI_STUB_CLASSES = [
    // 窗口 / 控件
    "QWidget", "QMainWindow", "QDialog", "QLabel", "QPushButton", "QLineEdit", "QTextEdit",
    "QCheckBox", "QRadioButton", "QComboBox", "QSpinBox", "QSlider", "QProgressBar", "QGroupBox",
    "QListWidget", "QListWidgetItem", "QTableWidget", "QTableWidgetItem", "QTableWidgetSelectionRange",
    "QTreeWidget", "QTreeWidgetItem", "QTabWidget", "QScrollArea", "QSplitter", "QStackedWidget",
    "QToolButton", "QStatusBar", "QToolBar", "QMenuBar", "QMenu", "QAction", "QActionGroup",
    "QShortcut", "QKeySequence", "QHeaderView", "QAbstractItemView", "QStyledItemDelegate",
    "QTableWidgetHeader", "QFrame", "QAbstractButton", "QButtonGroup", "QDateTimeEdit", "QDateEdit",
    "QTimeEdit", "QCalendarWidget", "QTextBrowser", "QPlainTextEdit", "QListView", "QAbstractSpinBox",
    "QDialogButtonBox", "QWizard", "QMdiArea", "QDockWidget", "QSplashScreen", "QSystemTrayIcon",
    "QDesktopWidget", "QApplicationWindow",
    // 布局
    "QLayout", "QBoxLayout", "QVBoxLayout", "QHBoxLayout", "QGridLayout", "QFormLayout",
    "QStackedLayout", "QSpacerItem",
    // 绘图
    "QPainter", "QPixmap", "QImage", "QIcon", "QGraphicsScene", "QGraphicsView", "QGraphicsItem",
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

// 控件基类需要接受 `QWidget.call(this, parent)` 语义, 上面的工厂已支持。
// Ui_BTEServerClass: uic 生成的界面类(由 BTEServer.ui 生成), 仓库里还没有对应 JS 文件,
// 这里提供一个全局空壳, 使"非界面代码"仍然可以构造 BTEServer 并跑起来。
g_globals.Ui_BTEServerClass = makeQtStubClass("Ui_BTEServerClass", {});

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
