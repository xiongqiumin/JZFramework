// 03_fs_path —— 文件与路径: fs(同步 API, 对齐 Node) + path + os + process。
//
// 编译跑起来:  <tsc> -p tsconfig.json
//              code_runtime --entry <本目录>/build/03_fs_path.js
//
// 两点要记牢:
//   * fs 的相对路径按**当前工作目录**解析 = 入口 js 所在的 build/ 目录, 所以想把东西写到
//     别处就用绝对路径; 需要一个临时目录就用 os.tmpdir() 起手;
//   * path 是纯字符串处理, 分隔符统一是 '/'(引擎内部路径也都归一化成 '/', 所以 path.sep 也是 '/',
//     join/normalize/resolve 的输出跟 path.sep 一致); ⚠ 但 path 识别不了 Windows 盘符绝对路径
//     (path.join('C:/x', 'y') 不会保留成正常的绝对路径), 所以带盘符的路径请用 path.sep 自己拼,
//     path.join 只喂相对路径。
//
// TS 侧的一个注意: 内置包的类型是 `declare module '<名字>'` 形式的 ambient 声明, 随工程分发的
// tsgo(TS 7) **不给这种导出函数的回调形参做上下文推导** —— 回调形参得自己写类型(下面的 d: Dirent),
// 类型也从同一个包名里 `import type` 取。

import fs from 'fs';
import type { Dirent } from 'fs';
import os from 'os';
import path from 'path';
import process from 'process';
import { check } from './helper/assert.js';

console.log('--- 03_fs_path ---');
console.log('cwd=' + process.cwd() + ' platform=' + os.platform() + ' sep=' + path.sep);

// 实验场放在系统临时目录: 跑之前清场, 跑完删掉, 不在源码树里留垃圾
const dir = os.tmpdir() + path.sep + 'jz_example_fs';
function under(name: string): string { return dir + path.sep + name; }   // 绝对路径自己接(见文件头那条注意)

if (fs.existsSync(dir))
    fs.rmSync(dir, { recursive: true });
fs.mkdirSync(dir, { recursive: true });

// ---- 1) 整文件读写 ----
const file = under('hello.txt');
fs.writeFileSync(file, 'hello', 'utf8');
check('写完之后存在', fs.existsSync(file));
check('按 utf8 读回文本', fs.readFileSync(file, 'utf8') === 'hello');

fs.appendFileSync(file, '-world');
check('追加写', fs.readFileSync(file, 'utf8') === 'hello-world');

const st = fs.statSync(file);
check('statSync: 是文件且大小为 11', st.isFile() && st.size === 11);

const bytes = fs.readFileSync(file);            // 不给编码 -> Uint8Array(重载靠参数类型区分)
check('按字节读回 Uint8Array', bytes instanceof Uint8Array && bytes.length === 11);

// ---- 2) 目录 ----
fs.mkdirSync(under('sub'), { recursive: true });
fs.writeFileSync(under('sub') + path.sep + 'a.txt', 'A');
const names = fs.readdirSync(dir);
check('readdirSync 能看到 sub', names.indexOf('sub') >= 0);
check('readdirSync 能看到 hello.txt', names.indexOf('hello.txt') >= 0);

const dirents = fs.readdirSync(dir, { withFileTypes: true });
const subs = dirents.filter(function (d: Dirent) { return d.isDirectory(); });
check('withFileTypes: 目录项带类型', subs.length === 1 && subs[0].name === 'sub');

fs.copyFileSync(file, under('sub') + path.sep + 'hello-copy.txt');
check('copyFileSync', fs.readFileSync(under('sub') + path.sep + 'hello-copy.txt', 'utf8') === 'hello-world');

// ---- 3) fd 级读写(二进制: 自己管打开/写/关闭) ----
const fd = fs.openSync(under('frame.bin'), 'w');
check('openSync 返回 fd(>= 3)', typeof fd === 'number' && fd >= 3);
fs.writeSync(fd, Uint8Array.fromHex('0102ff'));
fs.closeSync(fd);
check('fd 读回二进制一致', fs.readFileSync(under('frame.bin')).toHex() === '0102ff');

// ---- 4) path(纯字符串; 只喂相对路径) ----
check('path.join', path.join('a', 'b', 'c.txt') === 'a/b/c.txt');
check('path.basename', path.basename('/a/b/c.txt') === 'c.txt');
check('path.dirname', path.dirname('/a/b/c.txt') === '/a/b');
check('path.extname', path.extname('/a/b/c.txt') === '.txt');
check('path.isAbsolute', path.isAbsolute('/a/b') === true && path.isAbsolute('a/b') === false);
check('path.normalize', path.normalize('/a//b/../c') === '/a/c');

// ---- 5) 收摊 ----
fs.rmSync(dir, { recursive: true });
check('递归删除后不存在', !fs.existsSync(dir));

console.log('03_fs_path done');
