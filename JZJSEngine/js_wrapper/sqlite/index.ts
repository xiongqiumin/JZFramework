/**
 * @jz/sqlite —— 内部实现 (internal definition)
 *
 * 把引擎注入的 __jz.sqlite 原子 API 包装成 Node.js 内置模块 `node:sqlite` 风格的
 * 同步 SQLite 封装:
 *
 *   import { DatabaseSync } from '../js_wrapper/sqlite/dist/index.js';
 *   const db = new DatabaseSync('app.db');
 *   db.exec('CREATE TABLE IF NOT EXISTS t(id INTEGER PRIMARY KEY, name TEXT)');
 *   db.prepare('INSERT INTO t(name) VALUES (?)').run('hello');
 *   console.log(db.prepare('SELECT * FROM t').all());
 *   db.close();
 *
 * 与 Node `node:sqlite` 的一致性 / 差异:
 *  - 全同步 API(与 node:sqlite 一致, 没有 Promise 版本);
 *  - DatabaseSync / StatementSync 两个类, 方法名对齐上游
 *    (exec / prepare / close / isOpen / isTransaction / run / get / all / iterate / columns);
 *  - 参数绑定支持位置参数 `?` 与具名参数 `:name` / `@name` / `$name`,
 *    具名可用裸名对象 `{ name: 1 }`(对齐上游 allowBareNamedParameters 默认值);
 *  - JS <-> SQLite 类型: null / number / bigint / boolean / string / Uint8Array(BLOB);
 *    返回值: null / number / string / Uint8Array;
 *  - 未实现(见 types/index.d.ts 说明): function/aggregate 自定义函数、loadExtension、
 *    Session/changeset、SQLTagStore、backup、setReturnArrays;
 *  - isTransaction 是只读属性(与上游一致, 事务用 exec('BEGIN') 手动开启)。
 *
 * 本文件由 tsc 编译为 dist/index.js, 由用户入口模块 import 时经 QuickJS 模块加载器
 * 被动加载(与 net/core/serialport 一致, 不在 initEngine 主动执行)。
 */
'use strict';

declare const __jz: any;

// ---------------------------------------------------------------------------
// 参数收集
// ---------------------------------------------------------------------------
// 判断一个值是否应被视为"具名参数集合"(末尾对象参数), 而不是 BLOB/数组等可绑定值。
function isBindableObject(v: any): boolean {
    if (v === null || typeof v !== 'object') return false;
    if (Array.isArray(v)) return false;
    if (v instanceof Uint8Array || v instanceof ArrayBuffer) return false;
    if (v instanceof Date) return false;
    return true;
}

// Node 约定: 末尾若是普通对象则视为具名参数集合, 其余按出现顺序作为位置参数。
function splitParams(args: any[]): { anon: any[]; named: any } {
    let named: any = null;
    let end = args.length;
    if (end > 0 && isBindableObject(args[end - 1])) {
        named = args[end - 1];
        end--;
    }
    const anon: any[] = [];
    for (let i = 0; i < end; i++) anon.push(args[i]);
    return { anon: anon, named: named };
}

function makeError(msg: string): Error {
    return new Error(msg);
}

// ---------------------------------------------------------------------------
// StatementSync
// ---------------------------------------------------------------------------
class StatementSync {
    _db: DatabaseSync;
    _native: any;
    _id: number;
    _closed: boolean;
    _sql: string;

    constructor(db: DatabaseSync, id: number, sql: string) {
        this._db = db;
        this._native = db._native;
        this._id = id;
        this._sql = sql;
        this._closed = false;
    }

    /** 编译这条语句时的原始 SQL */
    get sourceSQL(): string { return this._sql; }
    /** 同 sourceSQL(本实现不做参数展开, 保持一致值) */
    get expandedSQL(): string { return this._sql; }

    private _check(): void {
        if (this._closed) throw makeError('statement has been finalized');
        if (!this._db.isOpen) throw makeError('database is not open');
    }

    /** 列名列表(未执行也能拿到, 对齐上游 columns()) */
    columns(): any[] {
        this._check();
        return this._native.stmtColumns(this._id);
    }

    /** 执行语句, 返回 { changes, lastInsertRowid } */
    run(...args: any[]): any {
        this._check();
        const p = splitParams(args);
        return this._native.stmtRun(this._id, p.anon, p.named);
    }

    /** 返回第一行(对象)或 null */
    get(...args: any[]): any {
        this._check();
        const p = splitParams(args);
        return this._native.stmtGet(this._id, p.anon, p.named);
    }

    /** 返回所有行 */
    all(...args: any[]): any[] {
        this._check();
        const p = splitParams(args);
        return this._native.stmtAll(this._id, p.anon, p.named);
    }

    /** 逐行迭代(本实现一次性取回后返回数组迭代器, 对齐语义不追求流式) */
    iterate(...args: any[]): any {
        return this.all.apply(this, args)[Symbol.iterator]();
    }

    /** 未实现, 保留接口以对齐上游 */
    setReadBigInts(enabled: boolean): void {
        // no-op: 本实现 INTEGER 一律返回 number(超出安全整数范围时精度可能丢失)
    }

    /** 未实现, 保留接口以对齐上游 */
    setReturnArrays(enabled: boolean): void {
        // no-op: 本实现固定返回对象行(不返回值数组)
    }

    /** 释放预编译语句(释放后该对象不可再用) */
    close(): void {
        if (this._closed) return;
        this._closed = true;
        this._db._native.stmtFinalize(this._id);
    }
}

// ---------------------------------------------------------------------------
// DatabaseSync
// ---------------------------------------------------------------------------
class DatabaseSync {
    _native: any;
    _closed: boolean;

    constructor(path: any, options?: any) {
        this._native = __jz.sqlite.openDb(String(path), options || {});
        this._closed = false;
    }

    /** 打开的数据库路径 */
    get path(): string { return this._native.path; }

    /** 连接是否可用(close() 或底层被释放后为 false) */
    get isOpen(): boolean { return !this._closed && !!this._native.isOpen; }

    /** 是否处于显式事务中(exec('BEGIN') 之后) */
    get isTransaction(): boolean { return this.isOpen && !!this._native.isTransaction; }

    private _check(): void {
        if (this._closed) throw makeError('database is not open');
    }

    /** 执行一条或多条 SQL(不返回结果), 出错抛异常 */
    exec(sql: any): void {
        this._check();
        this._native.exec(String(sql));
    }

    /** 预编译一条语句(可多次执行, 执行前自动 reset) */
    prepare(sql: any): StatementSync {
        this._check();
        const text = String(sql);
        const id = this._native.prepare(text);
        return new StatementSync(this, id, text);
    }

    /** 关闭连接(会同时释放本连接创建的语句) */
    close(): void {
        if (this._closed) return;
        this._closed = true;
        this._native.close();
    }
}

// 底层 SQLite 库版本(如 "3.53.4"), 由原生模块提供
const version: string = __jz.sqlite.version();

const sqlite = {
    DatabaseSync: DatabaseSync,
    StatementSync: StatementSync,
    version: version,
};

export { DatabaseSync, StatementSync, version, sqlite };

// 默认导出 = 模块对象(与其它内置名一致: 具名导入是推荐写法, 但默认导入也不会炸)
export default sqlite;
