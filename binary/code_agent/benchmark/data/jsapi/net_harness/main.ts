// ============================================================================
//  [评测框架 · 请勿修改]
//  本文件由 setup 注入到工程 src/main.ts，负责：
//    1) 读取并解析输入（_input.txt 中的 "<文本> <端口>"）；
//    2) 调用你在 src/solution.ts 里实现的 solve(text, port)；
//    3) 把 solve 的返回值（一行标记字符串）输出到 stdout。
//  你（模型）只需要编写 src/solution.ts，不要改动本文件、也不要自行读取/解析输入或 console.log，
//  输入解析与输出由框架统一处理（"不要让模型去处理输入"）。
// ============================================================================
import fs from "fs";
import { solve } from "./solution";

function readInputText(): string
{
    const candidates = ["./_input.txt", "../_input.txt", "./../_input.txt"];
    for (const p of candidates)
    {
        try
        {
            if (fs.existsSync(p))
                return fs.readFileSync(p, "utf8");
        }
        catch (e)
        {
            // ignore
        }
    }
    return "";
}

function parseInput(raw: string): { text: string; port: number }
{
    const s = (raw || "").trim();
    const sp = s.lastIndexOf(" ");
    if (sp < 0)
        return { text: s, port: 0 };
    const port = parseInt(s.slice(sp + 1).trim(), 10);
    const text = s.slice(0, sp).trim();
    return { text, port: Number.isFinite(port) ? port : 0 };
}

async function main(): Promise<void>
{
    const { text, port } = parseInput(readInputText());
    const out = await solve(text, port);
    if (typeof out === "string")
        console.log(out.replace(/\r?\n/g, " "));
}

main();
