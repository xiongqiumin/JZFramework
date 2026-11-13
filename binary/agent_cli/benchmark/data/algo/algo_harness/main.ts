// ============================================================================
//  [评测框架 · 请勿修改]
//  本文件由 setup 注入到工程 src/main.ts，负责：
//    1) 读取测试数据（testdata.json 里的 inputs，每个用例一个输入）；
//    2) 对每个输入调用你在 src/solution.ts 里实现的 solve(input)；
//    3) 把返回值按顺序收集，写出 ./result.json（{"outputs":[...]}）。
//  你（模型）只需要编写 src/solution.ts，不要改动本文件、也不要自行读写文件
//  （输入读取与结果写出由框架统一处理，“不要让模型去处理输入”）。
// ============================================================================
import fs from "fs";
import { solve } from "./solution";

function readTestData(): { problem: string; inputs: any[] }
{
    const candidates = ["./testdata.json", "../testdata.json"];
    for (const p of candidates)
    {
        try
        {
            if (fs.existsSync(p))
                return JSON.parse(fs.readFileSync(p, "utf8"));
        }
        catch (e)
        {
            // ignore
        }
    }
    return { problem: "", inputs: [] };
}

function main(): void
{
    const td = readTestData();
    const inputs: any[] = td.inputs || [];
    const outputs: any[] = inputs.map((x) => solve(x));
    fs.writeFileSync("./result.json", JSON.stringify({ outputs }));
}

main();
