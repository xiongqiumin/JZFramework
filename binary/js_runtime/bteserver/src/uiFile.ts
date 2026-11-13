// ---------------------------------------------------------------------------
// uiFile —— 界面 XML 的加载入口。
//
// `src/ui/` 下每个界面**只有一份 .ui**(引擎方言 XML)。它是原版 Qt 的 .ui 经转换器
// (tools/ui2jz.py, 已不在仓库里)转出来的, 现在这份就是唯一源头 —— 改界面直接改它。
//
// 为什么不是 import 内联字符串: 原来是 `ui/BTESERVER_UI_XML.ts` 把同一份 XML 内联成
// 字符串常量再 import, 等于同一份内容存两份(.ui + .ts)。现在改成运行期加载这一份 .ui。
//
// 路径怎么找: tsc 只编 .ts, **不会**把 .ui 拷进 build/ ⇒ 按顺序试两个位置:
//   1) <入口脚本目录>/ui/X.ui         —— .ui 与 main.js 一起部署到位时(发布形态)
//   2) <入口脚本目录>/../src/ui/X.ui  —— 源码树形态(直接跑 build/main.js 时)
// 引擎没有 __dirname; applicationDirPath() = 入口脚本(build/main.js)所在目录, 与
// main.ts 读 option.ini、netPackDefine.ts 读 Modules/BTELogCode.txt 是同一个口径。
// ---------------------------------------------------------------------------
import fs from 'fs';

export function loadUiFile(fileName: string): any
{
    const dir = jzApp.applicationDirPath();
    const candidates = [dir + '/ui/' + fileName, dir + '/../src/ui/' + fileName];

    for (const path of candidates)
    {
        if (fs.existsSync(path))
            return uiLoader.loadFile(path);
    }

    throw new Error("[uiFile] 找不到界面文件 " + fileName + "(找过: " + candidates.join(" | ") + ")");
}
