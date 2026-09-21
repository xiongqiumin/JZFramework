// JZLoopDetect 单测(纯逻辑: 不需要模型 / 网络 / 事件循环)
//
// 覆盖:
//   工具循环 —— 签名归一化(键序/嵌套/坏 JSON) / 同参数同结果(更早命中) /
//               同参数不同结果(容忍合法轮询) / 参数一直变(永不命中) / 窗口滑动 / reset / 开关
//   输出循环 —— 同一行刷屏 / 同一段反复 / 周期串复读 / 分隔线不误判 / 正常文本不误判 / 检查频率
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
    check(sig("read_file", "{\"a\":1}") != sig("search_file", "{\"a\":1}"),
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

    // ---- 2) 同参数 + 同结果: 第 2 次即判定(强信号) ----
    printf("\n同参数同结果(强信号):\n");
    {
        JZToolLoopDetector det;
        const string args = "{\"path\":\"src/main.ts\"}";
        const string out = "import ...";

        JZToolLoopEvent e1 = det.step("read_file", args, out);
        JZToolLoopEvent e2 = det.step("read_file", args, out);
        JZToolLoopEvent e3 = det.step("read_file", args, out);

        check(!e1.loop, "第 1 次: 不判定");
        check(e2.loop && e2.repeat == 2 && e2.sameResult == 2,
              "第 2 次: 命中(窗口内重复 2 次且结果相同)");
        check(e3.loop == false, "命中过之后不再重复判定");
        check(e2.message.find("read_file") != string::npos &&
              e2.message.find("src/main.ts") != string::npos,
              "错误文案带工具名与参数");
        check(e2.message.find("\n") == string::npos, "错误文案是单行");
    }

    // ---- 3) 同参数 + 结果不同: 容忍合法轮询, 第 3 次才判定 ----
    printf("\n同参数不同结果(合法轮询的容忍):\n");
    {
        JZToolLoopDetector det;
        const string args = "{\"run_id\":\"r1\"}";

        check(!det.step("terminal_query", args, "still running #0").loop, "第 1 次: 不判定");
        check(!det.step("terminal_query", args, "still running #1").loop, "第 2 次: 不判定");
        JZToolLoopEvent e3 = det.step("terminal_query", args, "still running #2");
        check(e3.loop && e3.repeat == 3 && e3.sameResult == 1,
              "第 3 次: 命中(只是重复, 结果并不一样)");
    }

    // ---- 4) 参数一直变: 永不判定 ----
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

    // ---- 5) 窗口滑动 ----
    printf("\n窗口滑动:\n");
    {
        // 序列 A X1 X2 A X3 A (每步结果都不同, 免得"结果相同"那条更强的规则先命中):
        //   不限窗口 -> 第 6 步时 A 出现 3 次 => 命中
        //   窗口=4   -> 第 6 步时窗口里只剩 2 次 A => 不判定
        JZToolLoopDetector limited;
        JZLoopConfig lc;
        lc.window = 4;
        limited.setConfig(lc);

        JZToolLoopDetector unlimited;
        JZLoopConfig uc;
        uc.window = 0;   // 不限窗口
        unlimited.setConfig(uc);

        const char *names[6] = { "A", "X1", "X2", "A", "X3", "A" };
        JZToolLoopEvent el, eu;
        for (int i = 0; i < 6; i++)
        {
            string out = "output-" + to_string(i);
            el = limited.step(names[i], "{}", out);
            eu = unlimited.step(names[i], "{}", out);
        }
        check(!el.loop && el.repeat == 2, "窗口=4: 最早的 A 已被挤出, 只数到 2 次");
        check(eu.loop && eu.repeat == 3, "不限窗口: 数到 3 次 A => 命中");
    }

    // ---- 6) 开关 / 阈值 / reset ----
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

    printf("\n%s (fail=%d)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
