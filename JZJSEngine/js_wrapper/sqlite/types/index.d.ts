/**
 * @jz/sqlite —— 外部定义 (external definition, public contract)
 *
 * 对齐 Node.js 内置模块 `node:sqlite` 的同步 API 子集。运行时由引擎加载
 * dist/index.js 提供, 底层是 JZDatabase(SQLite amalgamation) + JZJSqliteModule。
 *
 * 与上游 `node:sqlite` 的差异(未实现部分):
 * - database.function() / aggregate()    自定义 SQL 函数
 * - database.loadExtension()             loadable extension
 * - Session / changeset / applyChangeset 增量同步
 * - SQLTagStore()                        模板字符串标签语句缓存
 * - sqlite.backup()                      在线备份(唯一的上游异步 API)
 * - statement.setReturnArrays(true)      行以值数组返回(本实现固定返回对象)
 * - statement.setReadBigInts(true)       本实现 INTEGER 一律返回 number
 * - 所有 API 均为同步(与上游一致, 仅 backup 是异步)
 */

/** 可以绑定到语句的参数值 */
export type SQLInputValue = null | number | bigint | boolean | string | Uint8Array | ArrayBuffer;

/** 查询结果里的值 */
export type SQLOutputValue = null | number | string | Uint8Array;

/** 具名参数集合: 键可写裸名(name)也可带前缀(:name / @name / $name) */
export type SQLNamedParameters = Record<string, SQLInputValue>;

export interface DatabaseOptions {
  /** 连接可用性(Node 里用于延迟 open; 本实现总是立即打开) */
  open?: boolean;
  /** 只读打开, 默认 false */
  readOnly?: boolean;
  /** 是否开启外键约束, 默认 true(与上游一致) */
  enableForeignKeyConstraints?: boolean;
  /** 忙等超时(毫秒), 默认 0 = 不等待 */
  timeout?: number;
}

/** run() 的返回 */
export interface StatementResultingChanges {
  /** 本次语句影响的行数 */
  changes: number;
  /** 最后一次插入的 rowid */
  lastInsertRowid: number;
}

/**
 * 预编译语句。只能由 database.prepare() 创建, 不可直接 new。
 * 同一条语句可重复执行(每次执行前自动 reset 并清除绑定)。
 */
export class StatementSync {
    /** 编译时的原始 SQL */
    readonly sourceSQL: string;
    /** 与 sourceSQL 相同(本实现不做参数展开) */
    readonly expandedSQL: string;

    /** 列名列表(上游返回的是列元数据对象数组, 本实现只给出列名) */
    columns(): string[];

    /** 执行语句, 返回变更行数与最后插入 rowid */
    run(...anonymousParameters: SQLInputValue[]): StatementResultingChanges;
    run(namedParameters: SQLNamedParameters, ...anonymousParameters: SQLInputValue[]): StatementResultingChanges;

    /** 执行查询并返回第一行(对象), 无结果返回 null */
    get(...anonymousParameters: SQLInputValue[]): Record<string, SQLOutputValue> | null;
    get(namedParameters: SQLNamedParameters, ...anonymousParameters: SQLInputValue[]): Record<string, SQLOutputValue> | null;

    /** 执行查询并返回所有行 */
    all(...anonymousParameters: SQLInputValue[]): Record<string, SQLOutputValue>[];
    all(namedParameters: SQLNamedParameters, ...anonymousParameters: SQLInputValue[]): Record<string, SQLOutputValue>[];

    /** 逐行迭代(本实现一次性取回后返回迭代器) */
    iterate(...anonymousParameters: SQLInputValue[]): IterableIterator<Record<string, SQLOutputValue>>;

    /** 未实现(no-op): INTEGER 一律返回 number */
    setReadBigInts(enabled: boolean): void;
    /** 未实现(no-op): 固定返回对象行 */
    setReturnArrays(enabled: boolean): void;

    /** 释放预编译语句 */
    close(): void;
}

/**
 * 一个 SQLite 数据库连接(全同步)。
 *
 * 路径按进程当前工作目录解析(与 fs 一致); ':memory:' 为内存数据库。
 * 连接由 GC 兜底回收(析构时自动关闭), 但仍建议显式 close()。
 */
export class DatabaseSync {
    constructor(path: string, options?: DatabaseOptions);

    /** 打开的数据库路径 */
    readonly path: string;
    /** 连接是否可用 */
    readonly isOpen: boolean;
    /** 是否处于显式事务中(exec('BEGIN') 之后, COMMIT/ROLLBACK 之前) */
    readonly isTransaction: boolean;

    /** 执行一条或多条 SQL, 不返回结果; 出错抛异常 */
    exec(sql: string): void;

    /** 预编译一条语句 */
    prepare(sql: string): StatementSync;

    /** 关闭连接(同时释放该连接创建的所有语句) */
    close(): void;
}

/** 底层 SQLite 库版本, 例如 "3.53.4" */
export const version: string;

export const sqlite: {
    DatabaseSync: typeof DatabaseSync;
    StatementSync: typeof StatementSync;
    version: string;
};
