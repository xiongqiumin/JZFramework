// @jz/widget —— JZDateEdit 的 **Qt 形状**方法(与内建 Date 的换算只在这一层做)。
//
// 为什么换算放包装层, 不放 C++ 绑定:
//   C++ 侧只认 JZDateTime(毫秒时间戳), 让它去认 JS 的 Date 对象得在绑定层掏 getTime()、
//   造 JS Date; 而 Qt 移植过来的调用点写的是 `setDate(new Date())` / `dateTime()` —— 这是
//   **JS 侧的形状问题**, 交给包装层最省事(与 menus.ts 在 TS 侧实现 exec 同一个理由)。
//
// 装好之后(与 Qt QDateEdit 的用法一致):
//   const ed = new JZDateEdit();
//   ed.setDate(new Date(2026, 9, 4));   // 也接受毫秒时间戳
//   ed.dateTime();                       // -> Date
//   ed.date();                           // -> Date (同 dateTime; Qt 的 QDateEdit::date)
//   ed.onDateChanged = (d) => log(d);    // d 是 Date
//
// 注: C++ 侧的日期模型还是骨架(解析/微调/弹出日历是 TODO, 见 JZWidget/JZDateEdit.h);
//     这里只是把 msecs 接口包成 Date 形状, 日期"未设置"时会读回 1970-01-01(Qt 用无效 QDate)。
export function installDateEditHelpers(): void
{
    if (typeof JZDateEdit === 'undefined')
        return;

    const p = JZDateEdit.prototype as any;
    if (p.__jzDateHelpers)
        return;                     // 幂等: 本模块可能被求值两次(见 classes.ts 末尾说明)
    p.__jzDateHelpers = true;

    p.setDate = function (this: any, v: Date | number): void
    {
        const ms = (v instanceof Date) ? v.getTime() : Number(v);
        if (!Number.isFinite(ms))
            throw new TypeError('JZDateEdit.setDate: 需要 Date 或毫秒时间戳');
        this.setDateMsecs(ms);
    };

    p.dateTime = function (this: any): Date
    {
        return new Date(this.dateMsecs());
    };

    // Qt 的 QDateEdit::date 返回 QDate(只有年月日); JS 侧统一给 Date, 时间部分为当天 00:00 的
    // 时间戳原样保留(C++ 侧 JZDateTime 也只存时间戳)。
    p.date = p.dateTime;
}

installDateEditHelpers();
