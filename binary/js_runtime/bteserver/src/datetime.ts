// ---------------------------------------------------------------------------
// datetime.js —— 时间工具(取代 Qt 的 QDateTime / QDate / QTime)
//
// 原 C++ 用 QDateTime/QDate/QTime 三种类型; JS 原生只有一种: **Date(毫秒时间戳)**。
// 本文件只补"Qt 有而 JS 没有"的两件事, 不引入任何包装类型:
//
//   1) Qt 的日期格式串。JS 没有 strftime, 手工拼接又到处重复, 故收口成 formatDateTime。
//      支持: yyyy yy MM dd HH hh mm ss zzz, 以及 [..] 里的字面量(Qt 的转义写法)。
//   2) QDateTime 的"语义方法"(它们只是时间戳减法, 但语义要跟 Qt 对齐):
//        addSecs(n)  -> addSeconds(date, n)
//        secsTo(o)   -> secondsBetween(a, b)   (b - a, 秒)
//        daysTo(o)   -> daysBetween(a, b)      (按**日历天**算, 与 QDate::daysTo 一致)
//        lt(o)/gt(o) -> JS 里直接 `a < b` (Date 参与比较是按 valueOf)
//        isNull()    -> 变量直接用 null 表示"无效/未设置"
//
// 约定: 所有"未设置"的时间一律用 **null**(原 C++ `QDateTime()` 默认构造出来就是无效值,
//       JS 的 `new Date()` 拿到的是**当前时间**, 两者语义相反 —— 这是最容易写错的一处)。
// ---------------------------------------------------------------------------

/** Qt 日期格式串 -> 字符串。fmt 省略时用 "yyyy-MM-dd HH:mm:ss"。 */
export function formatDateTime(date, fmt)
{
    if (!(date instanceof Date) || isNaN(date.getTime()))
        return "";

    const f = (fmt === undefined || fmt === null) ? "yyyy-MM-dd HH:mm:ss" : String(fmt);

    // 先保护 [..] 字面量(连方括号一起用 \u0001 包起来), 免得里面的 yyyy/mm 被当格式符替换。
    // 注意要包 **整个匹配 m**(含方括号): 只包捕获组的话, 最后还原时会把 [ ] 丢掉。
    const guarded = f.replace(/\[([^\]]*)\]/g, (m) => "\u0001" + m + "\u0001");

    const out = guarded.replace(/yyyy|yy|MM|dd|HH|hh|mm|ss|zzz/g, (t) =>
    {
        switch (t)
        {
            case "yyyy": return String(date.getFullYear());
            case "yy": return pad2(date.getFullYear() % 100);
            case "MM": return pad2(date.getMonth() + 1);
            case "dd": return pad2(date.getDate());
            case "HH": return pad2(date.getHours());
            case "hh": return pad2(date.getHours() % 12 || 12);
            case "mm": return pad2(date.getMinutes());
            case "ss": return pad2(date.getSeconds());
            case "zzz": return pad3(date.getMilliseconds());
            default: return t;
        }
    });

    return out.replace(/\u0001([^\u0001]*)\u0001/g, (m, lit) => lit);
}

/** QDateTime::addSecs(n) */
export function addSeconds(date, n)
{
    return new Date(date.getTime() + n * 1000);
}

/** QDateTime::addDays(n) */
export function addDays(date, n)
{
    return new Date(date.getTime() + n * 86400000);
}

/**
 * 把"可能不是 Date"的值归一成 Date(否则 null)。
 * 用途: 日期选择控件(QDateTimeEdit/QDateEdit)还没移植, 空壳返回的不是 Date;
 *       这里统一收口, 让调用点的语义明确 —— 拿不到有效时间就当"未设置"(不筛选/不比较)。
 */
export function asDate(v)
{
    if (v instanceof Date)
        return isValidDate(v) ? v : null;
    if (v && typeof v.getTime === "function")
    {
        const d = new Date(v.getTime());
        return isValidDate(d) ? d : null;
    }
    return null;
}

/**
 * QDateTime::secsTo(o): 从 a 到 b 的秒数(b 在后为正)。
 * Qt 里任一操作数无效(默认构造的 QDateTime / null)时返回 **0**, 这里保持一致 ——
 * 业务代码正是靠这个语义表达"还没发过包/还没收到心跳时不要触发超时"。
 */
export function secondsBetween(a, b)
{
    if (!isValidDate(a) || !isValidDate(b))
        return 0;
    return Math.round((b.getTime() - a.getTime()) / 1000);
}

/**
 * QDateTime::daysTo(o) / QDate::daysTo(o): 按**日历天**算, 与 Qt 一致
 * (23:00 -> 次日 01:00 = 1 天)。任一操作数无效 -> 0(同 Qt)。
 */
export function daysBetween(a, b)
{
    if (!isValidDate(a) || !isValidDate(b))
        return 0;
    const d1 = new Date(a.getFullYear(), a.getMonth(), a.getDate());
    const d2 = new Date(b.getFullYear(), b.getMonth(), b.getDate());
    return Math.round((d2.getTime() - d1.getTime()) / 86400000);
}

// QDateTime::operator< / operator> —— 无效(null)时**一律 false**。
// 不能直接用 JS 的 `null < date`(null 会转成 0, 结果恒为 true)。
export function isBefore(a, b)
{
    return isValidDate(a) && isValidDate(b) && a.getTime() < b.getTime();
}
export function isAfter(a, b)
{
    return isValidDate(a) && isValidDate(b) && a.getTime() > b.getTime();
}

/** 时间是否有效(对应 QDateTime::isValid(); 变量为 null 即 Qt 的 isNull()) */
export function isValidDate(date)
{
    return (date instanceof Date) && !isNaN(date.getTime());
}

export function pad2(v) { return String(v).padStart(2, "0"); }
export function pad3(v) { return String(v).padStart(3, "0"); }
