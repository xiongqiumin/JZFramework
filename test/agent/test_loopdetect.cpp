// JZLoopDetect 单测(纯逻辑: 不需要模型 / 网络 / 事件循环)
//
// 覆盖:
//   工具循环 —— 签名归一化(键序/嵌套/坏 JSON) / 同参数同结果(连续攒够阈值才判) /
//               同参数不同结果 / 轮询类工具单独放宽 / 参数一直变(永不命中) /
//               只认连续重复(夹杂别的调用即断链, 含 build 误报回归) / window 内存上限 / reset / 开关
//   输出循环 —— 同一行刷屏 / 同一段反复 / 周期串复读 / n-gram 重复率(长单元 & 改写型) /
//               分隔线不误判 / 正常文本不误判 / 检查频率
// 期望输出: ALL PASS
//
// 注: 本文件只测判定逻辑 —— 判定结果如何被用(报 LoopError、丢弃半截回复)在 JZAgent 里,
//     它没有离线宿主(需要真实模型), 那部分靠编译 + 端到端冒烟。

#include <stdio.h>
#include <string>

#include "JZLoopDetect.h"

using namespace std;

static int g_fail = 0;

static void check(bool ok, const string &what)
{
    printf("%s %s\n", ok ? "[ok]  " : "[fail]", what.c_str());
    if (!ok)
        g_fail++;
}

static string sig(const string &name, const string &args)
{
    return JZToolLoopDetector::signature(name, args);
}

static string rep(const string &unit, int n)
{
    string out;
    for (int i = 0; i < n; i++)
        out += unit;
    return out;
}

// 前缀补足到 >= 256 字节, 保证过了"累计新增够多才检查"的门槛
// (末尾补空行: 前缀本身是一整段, 不并进尾部那段, 免得干扰"同段重复"的判定)
static string pad(const string &tail)
{
    string head = "下面是任务的处理说明与结论草稿, 内容正常且各不相同, 先凑够长度再看尾部:";
    while (head.size() < 256)
        head += " 第" + to_string(head.size()) + "句说明内容。";
    return head + "\n\n" + tail;
}

int main()
{
    printf("== JZToolLoopDetector ==\n\n");

    // ---- 1) 签名归一化 ----
    printf("签名归一化:\n");
    check(sig("read_file", "{\"b\":1,\"a\":2}") == sig("read_file", "{\"a\":2,\"b\":1}"),
          "参数键序不同 => 同一个签名");
    check(sig("read_file", "{\"a\":1}") != sig("read_file", "{\"a\":2}"),
          "参数值不同 => 不同签名");
    check(sig("read_file", "{\"a\":1}") != sig("search_context", "{\"a\":1}"),
          "工具名不同 => 不同签名");
    check(sig("terminal_run", "ls  -l\n") == sig("terminal_run", "ls -l"),
          "参数不是合法 JSON => 折叠空白后比较");
    check(sig("read_file", "{\"p\":{\"y\":1,\"x\":2}}") == sig("read_file", "{\"p\":{\"x\":2,\"y\":1}}"),
          "嵌套对象的键序也归一化");
    check(sig("t", "") == sig("t", "   "), "空参数 == 全空白参数");
    check(JZToolLoopDetector::argsPreview("{\n  \"path\": \"a.ts\"\n}").find("\n") == string::npos,
          "参数简写: 换行折叠成单行");
    check(JZToolLoopDetector::argsPreview(string(300, 'x'), 10) == string(10, 'x') + "…",
          "参数简写: 超长截断");

    // ---- 2) 同参数 + 同结果: 连续攒够主阈值(默认 5)才判 ----
    //   ⚠ 曾经是"第 2 次即判"的强信号, 但那会误伤无参数 + 结果恒定的工具(如 build) ——
    //     见下面 6b) 的回归用例。现在 sameResult 默认与 repeat 同值(5)。
    printf("\n同参数同结果(连续重复):\n");
    {
        JZToolLoopDetector det;
        const string args = "{\"path\":\"src/main.ts\"}";
        const string out = "import ...";

        JZToolLoopEvent e1 = det.step("read_file", args, out);
        JZToolLoopEvent e2 = det.step("read_file", args, out);
        JZToolLoopEvent e3 = det.step("read_file", args, out);
        JZToolLoopEvent e4 = det.step("read_file", args, out);
        JZToolLoopEvent e5 = det.step("read_file", args, out);
        JZToolLoopEvent e6 = det.step("read_file", args, out);

        check(!e1.loop && !e2.loop && !e3.loop && !e4.loop,
              "第 1~4 次: 不判定(阈值 5)");
        check(e5.loop && e5.repeat == 5 && e5.sameResult == 5,
              "第 5 次: 命中(连续 5 次同签名且结果相同)");
        check(e6.loop == false, "命中过之后不再重复判定");
        check(e5.message.find("read_file") != string::npos &&
              e5.message.find("src/main.ts") != string::npos,
              "错误文案带工具名与参数");
        check(e5.message.find("\n") == string::npos, "错误文案是单行");
        check(e5.message.find("连续") != string::npos, "错误文案标明是连续调用");
    }

    // ---- 3) 同参数 + 结果不同: 普通工具攒够主阈值(默认 5)才判 ----
    //   "改一版编一次"这种正常流程里, 同一条命令跑 3~4 次是常事, 不该被打成打转。
    printf("\n同参数不同结果(等编译/等运行这类重复):\n");
    {
        JZToolLoopDetector det;
        const string args = "{\"command\":\"tsc -p tsconfig.json\"}";
        for (int i = 1; i <= 4; i++)
            check(!det.step("terminal_run", args, "输出第 " + to_string(i) + " 版").loop,
                  "第 " + to_string(i) + " 次: 不判定(改一次编一次是正常流程)");

        JZToolLoopEvent e5 = det.step("terminal_run", args, "输出第 5 版");
        check(e5.loop && e5.repeat == 5 && e5.sameResult == 1,
              "第 5 次: 命中(同签名重复 5 次, 结果并不一样)");
    }

    // ---- 4) 轮询类工具: 同参数反复查进度是正常用法, 走单独放宽的阈值 ----
    printf("\n轮询类工具(terminal_query):\n");
    {
        JZToolLoopDetector det;
        const string args = "{\"run_id\":\"t1-r1\"}";

        bool early = false;
        for (int i = 1; i <= 15; i++)
        {
            // 结果每次都在变(后台任务的日志在涨) —— 这正是"查进度"的常态
            if (det.step("terminal_query", args, "still running #" + to_string(i), true).loop)
                early = true;
        }
        check(!early, "轮询 15 次(结果一直在变): 不判定");

        JZToolLoopEvent e16 = det.step("terminal_query", args, "still running #16", true);
        check(e16.loop && e16.repeat == 16, "第 16 次: 命中(轮询阈值 pollRepeat=16)");
        check(e16.message.find("轮询类工具") != string::npos, "错误文案标明是轮询类工具");
        check(e16.message.find("\n") == string::npos, "错误文案是单行");

        // 豁免靠**工具自己声明**, 不是靠名字: 同一个名字不带标记就走主阈值
        JZToolLoopDetector plain;
        bool hit5 = false;
        for (int i = 1; i <= 5; i++)
            hit5 = plain.step("terminal_query", args, "x" + to_string(i)).loop;
        check(hit5, "不带轮询标记时按主阈值判定: 第 5 次命中");

        // 轮询工具"结果一直没变"仍要能拦住(真卡死): 走 pollSameResult=6
        JZToolLoopDetector stuck;
        int stuckAt = 0;
        for (int i = 1; i <= 8; i++)
        {
            if (stuck.step("terminal_query", args, "同样的一行输出", true).loop)
            {
                stuckAt = i;
                break;
            }
        }
        check(stuckAt == 6, "轮询工具结果一直相同: 第 6 次命中(pollSameResult=6), 实际 " +
                                to_string(stuckAt));
    }

    // ---- 5) 参数一直变: 永不判定 ----
    printf("\n参数一直变(模型在换做法):\n");
    {
        JZToolLoopDetector det;
        bool any = false;
        for (int i = 0; i < 30; i++)
        {
            if (det.step("read_file", "{\"path\":\"f" + to_string(i) + ".ts\"}", "body").loop)
                any = true;
        }
        check(!any, "30 次不同参数: 一次都不判定");
    }

    // ---- 6) 只认"连续重复": 中间夹了别的工具调用就断链 ----
    printf("\n只认连续重复(夹杂别的调用即断链):\n");
    {
        // 阈值显式定成 3, 免得默认值一改这条测试就跟着飘。
        JZLoopConfig c3;
        c3.repeat = 3;
        c3.sameResult = 3;

        // 序列 A X1 X2 A X3 A: 尾部连续段只有 1 次 A => 永不命中
        JZToolLoopDetector det;
        det.setConfig(c3);
        const char *names[6] = { "A", "X1", "X2", "A", "X3", "A" };
        bool any = false;
        for (int i = 0; i < 6; i++)
            any = det.step(names[i], "{}", "output-" + to_string(i)).loop || any;
        check(!any, "非连续的同名调用(夹杂别的工具): 一次都不判定");

        // 真连续: A A A => 第 3 次命中
        JZToolLoopDetector run;
        run.setConfig(c3);
        check(!run.step("A", "{}", "x").loop &&
              !run.step("A", "{}", "x").loop &&
              run.step("A", "{}", "x").loop,
              "连续 3 次同签名: 第 3 次命中");

        // window 现在只是内存上限: 保留的步数不超过 window
        JZToolLoopDetector cap;
        JZLoopConfig cc;
        cc.window = 4;
        cap.setConfig(cc);
        for (int i = 0; i < 10; i++)
            cap.step("t" + to_string(i), "{}", "o" + to_string(i));
        check(cap.stepCount() == 4, "window=4: 只保留最近 4 步(内存上限)");
    }

    // ---- 6b) 回归: 无参数 + 结果恒定的工具(如 build)不该被误判 ----
    //   实测(2026-10-08): `build` **无参数**(签名恒为 build|{}) + 成功文案是**常量**
    //   ⇒ 老实现里"同签名同结果 >= 2"必然成立, 于是 "build → 改文件 → build" 第 2 次就被判死,
    //     把"改代码→重编→再跑"这个正常迭代闭环误杀(aime25_02/03 因此 FAIL)。
    printf("\n回归: build 这类无参数/结果恒定的工具:\n");
    {
        const string ok = "编译通过，现在可以用 run_js 运行程序。";
        const char *seq[11] = { "build", "run_js", "terminal_run", "terminal_run", "run_js",
                                "terminal_run", "modify_file", "build", "run_js", "modify_file", "build" };
        JZToolLoopDetector det;
        bool any = false;
        for (int i = 0; i < 11; i++)
            any = det.step(seq[i], "{}", (i == 6) ? "已改写文件" : ok).loop || any;
        check(!any, "夹杂改文件/运行/查日志的 3 次 build: 一次都不判定(实测误报场景)");

        // 反过来: 真的连着 build 5 次(什么都没干) => 第 5 次命中
        JZToolLoopDetector stuck;
        int hitAt = 0;
        for (int i = 1; i <= 6; i++)
        {
            if (stuck.step("build", "{}", ok).loop)
            {
                hitAt = i;
                break;
            }
        }
        check(hitAt == 5, "真的连着 build 5 次: 第 5 次命中, 实际 " + to_string(hitAt));
    }

    // ---- 7) 开关 / 阈值 / reset ----
    printf("\n配置与 reset:\n");
    {
        JZToolLoopDetector off;
        JZLoopConfig c;
        c.enabled = false;
        off.setConfig(c);
        bool any = false;
        for (int i = 0; i < 20; i++)
        {
            if (off.step("read_file", "{}", "same").loop)
                any = true;
        }
        check(!any, "enabled=false: 一次都不判定");

        JZToolLoopDetector strict;
        JZLoopConfig sc;
        sc.repeat = 2;
        sc.sameResult = 2;
        strict.setConfig(sc);
        check(!strict.step("read_file", "{}", "a").loop, "自定义阈值: 第 1 次不判定");
        check(strict.step("read_file", "{}", "a").loop, "自定义阈值: 第 2 次即命中");

        JZToolLoopDetector det;
        det.step("read_file", "{}", "same");
        det.reset();
        JZToolLoopEvent after = det.step("read_file", "{}", "same");
        check(!after.loop && after.repeat == 1 && det.stepCount() == 1,
              "reset 后: 计数从 0 开始(不跨任务累积)");
    }

    printf("\n== JZTextLoopDetector ==\n\n");

    // ---- 7) 同一行刷屏 ----
    printf("同一行连续重复:\n");
    {
        JZTextLoopDetector det;
        JZTextLoopEvent e = det.check(pad(rep("我会尽快修复这个问题。\n", 8)), "思考");
        check(e.loop && e.repeat >= 6, "同一行刷 8 次 => 命中");
        check(e.message.find("思考") != string::npos, "错误文案标明是思考还是回复");
        check(e.message.find("\n") == string::npos, "错误文案是单行");
    }

    // ---- 8) 同一段反复(周期可能很长, 只有"段落"规则盖得住) ----
    printf("同一段反复:\n");
    {
        JZTextLoopDetector det;
        string block = "先检查 tsconfig 的 outDir 与 rootDir 设置是否一致, 再确认 main.ts 里"
                       "导出入口名字与用例期望的输出完全对应, 最后重新编译一次。";
        JZTextLoopEvent e = det.check(pad(block + "\n\n" + block + "\n\n" + block + "\n\n"), "回复");
        check(e.loop && e.repeat >= 3, "同一段连贴 3 次 => 命中");

        JZTextLoopDetector det2;
        check(!det2.check(pad(block + "\n\n" + block + "\n\n"), "回复").loop,
              "同一段只贴 2 次: 不判定");
    }

    // ---- 9) 周期串复读 ----
    printf("周期串复读:\n");
    {
        JZTextLoopDetector det;
        JZTextLoopEvent e = det.check(pad(rep("让我再想想", 20)), "思考");
        check(e.loop, "同一句复读 20 次 => 命中");

        JZTextLoopDetector det2;
        check(!det2.check(pad("短串 abc 只出现一次, 其余内容都各不相同。"), "思考").loop,
              "正常文本: 不判定");
    }

    // ---- 10) 不误判: 分隔线 / 收尾空行 ----
    printf("不误判:\n");
    {
        JZTextLoopDetector a;
        check(!a.check(pad(rep("-", 60)), "回复").loop, "长分隔线(同一字符): 不判定");

        JZTextLoopDetector b;
        check(!b.check(pad(rep("===\n", 10)), "回复").loop, "`===` 分隔线: 不判定");

        JZTextLoopDetector c;
        string varied;
        for (int i = 0; i < 40; i++)
            varied += "第 " + to_string(i) + " 步的说明内容不一样。\n";
        check(!c.check(pad(varied), "回复").loop, "正常多行文本: 不判定");
    }

    // ---- 11) 检查频率 + reset ----
    printf("检查频率与 reset:\n");
    {
        JZTextLoopDetector det;
        string text = rep("重复的内容。", 2);   // 36 字节 < textCheckMinChars(128)
        check(!det.check(text, "回复").loop, "不足一次检查量(128 字节): 这次不检查");
        check(det.lastCheckedLen() == 0, "没到检查量: 记录的长度保持 0");

        JZTextLoopEvent e = det.check(pad(rep("重复的内容。", 20)), "回复");
        check(e.loop, "补够长度后: 命中");
        det.reset();
        check(det.lastCheckedLen() == 0, "reset 后: 长度计数清零");
    }

    // ---- 12) 后缀重复率(n-gram): 补 ①②③ 漏掉的两类 ----
    //   ①②③ 都要求**逐字节完全相等**, 这里两种情形它们都不认:
    //     · 单元 500 字 > 周期上限(256) 且没有空行分隔 => ② 不认(不是独立段)、③ 不认(超周期)、① 只看单行;
    //     · 每遍稍微改几个字 => 连"完全相等"都不成立。
    printf("后缀重复率(n-gram):\n");
    {
        // ⚠ 这里**不用** pad(): pad 会掺进一段各不相同的长前缀, 把重复率稀释掉。
        //    check() 只要求累计 >= textCheckMinChars(128), 下面这段本身就有两千多字符。
        string unit;
        while (unit.size() < 500)
            unit += "把已知条件代入并逐项核对系数, 确认每一步的符号与单位都正确。";

        string text;
        for (int i = 0; i < 4; i++)
            text += unit + "\n";
        JZTextLoopDetector det;
        JZTextLoopEvent e = det.check(text, "思考");
        check(e.loop, "同一段(500 字)连贴 4 次(无空行/超周期上限): 命中");
        check(e.message.find("重复率") != string::npos, "文案说明是重复率过高");
        check(!e.sample.empty(), "文案带重复片语样本");
        check(e.message.find("\n") == string::npos, "错误文案是单行");

        JZTextLoopDetector det2;
        string rewritten;
        for (int i = 0; i < 4; i++)
        {
            string u = unit;
            u.replace(0, 4, "先把第" + to_string(i) + "种");
            rewritten += u + "\n";
        }
        check(det2.check(rewritten, "思考").loop, "同一段每遍只改几个字(不再逐字节相等): 命中");

        JZTextLoopDetector det3;
        string normal;
        for (int i = 0; i < 60; i++)
            normal += "第 " + to_string(i) + " 项核对的是 " + to_string(i * 137 % 977) + " 与 "
                      + to_string(i * 61 % 419) + " 的换算关系, 结论各不相同。\n";
        check(!det3.check(normal, "思考").loop, "正常长文(每句都不同): 不判定");

        JZTextLoopDetector det4;
        check(!det4.check(rep("只有一句重复的话。", 3), "思考").loop,
              "总量不足一次检查量/gram 下限: 不判定");
    }

    printf("\n%s (fail=%d)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
