// 08_sqlite —— 存数据: 创表 / 预编译语句 / 具名参数 / 事务 / 落到文件。
//
// 编译跑起来:  <tsc> -p tsconfig.json
//              code_runtime --entry <本目录>/build/08_sqlite.js
//
// 全部是**同步** API(对齐 node:sqlite): 没有 await, 一行一行往下写就行。
// ':memory:' 是内存库; 给文件路径就是文件库(第二次打开还能读到上次写的)。
//
// 类型上的一点: get() 的返回是 `Record<string, SQLOutputValue> | null`(查不到就是 null),
// 所以取值前得先判一下 —— 这就是下面几处 `x !== null && x.c === ...` 的来由。

import fs from 'fs';
import os from 'os';
import path from 'path';
import { DatabaseSync } from 'sqlite';
import { check } from './helper/assert.js';

console.log('--- 08_sqlite ---');

// ---- 1) 内存库 ----
const mem = new DatabaseSync(':memory:');
mem.exec('CREATE TABLE sample(id INTEGER PRIMARY KEY, name TEXT, temp REAL)');

const insert = mem.prepare('INSERT INTO sample(name, temp) VALUES(?, ?)');
const r1 = insert.run('pump-01', 36.5);
check('run() 返回 lastInsertRowid', r1.lastInsertRowid === 1);
check('run() 返回 changes', r1.changes === 1);
insert.run('fan-02', 42.1);

const rows = mem.prepare('SELECT * FROM sample ORDER BY id').all();
check('查到两行', rows.length === 2);
check('第一行内容', rows[0].name === 'pump-01' && rows[0].temp === 36.5);

// get() 只要第一行; 查不到是 null
const one = mem.prepare('SELECT * FROM sample WHERE name = ?').get('fan-02');
check('get() 命中', one !== null && one.name === 'fan-02');
const none = mem.prepare('SELECT * FROM sample WHERE name = ?').get('no-such-device');
check('get() 查不到是 null', none === null);

// 具名参数(:name 形式, 键可以不带前缀)
const over = mem.prepare('SELECT name, temp FROM sample WHERE temp > :limit');
check('具名参数查询', over.all({ limit: 40 }).length === 1);

// 事务: BEGIN / COMMIT(出错就 ROLLBACK)
mem.exec('BEGIN');
check('进入事务', mem.isTransaction === true);
for (let i = 1; i <= 3; i++)
    mem.prepare('INSERT INTO sample(name, temp) VALUES(?, ?)').run('batch-' + i, 20 + i);
mem.exec('COMMIT');
check('提交后不再是事务', mem.isTransaction === false);
const countRow = mem.prepare('SELECT COUNT(*) AS c FROM sample').get();
check('批量插入后的行数', countRow !== null && countRow.c === 5);

const ver = mem.prepare('SELECT 1 AS ok').get();
check('能执行任意 SQL', ver !== null && ver.ok === 1);
mem.close();
check('close 之后 isOpen = false', mem.isOpen === false);

// ---- 2) 文件库: 落盘 + 换个连接还能读出来 ----
// (同 03_fs_path: 带盘符的绝对路径别交给 path.join, 用 path.sep 自己接)
const dbFile = os.tmpdir() + path.sep + 'jz_example_sqlite.db';
if (fs.existsSync(dbFile))
    fs.unlinkSync(dbFile);

const db = new DatabaseSync(dbFile);
db.exec('CREATE TABLE alarm(ts INTEGER, msg TEXT)');
const addAlarm = db.prepare('INSERT INTO alarm(ts, msg) VALUES(?, ?)');
addAlarm.run(1700000000, 'pump-01 超温');
addAlarm.run(1700000060, 'pump-01 恢复');
check('文件库路径', db.path === dbFile);
db.close();

const reopen = new DatabaseSync(dbFile);
const alarms = reopen.prepare('SELECT * FROM alarm ORDER BY ts').all();
check('重开之后数据还在', alarms.length === 2 && alarms[0].msg === 'pump-01 超温');
reopen.close();

fs.unlinkSync(dbFile);
check('删掉临时库文件', !fs.existsSync(dbFile));

console.log('08_sqlite done');
