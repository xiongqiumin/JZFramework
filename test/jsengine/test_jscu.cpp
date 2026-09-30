// ==================== computer-use: JS 侧端到端(虚拟桌面) ====================
// 验证的是**脚本真正跑起来的那条链**:
//
//   TS 包(js_wrapper/computeruse/dist) → __jz.computeruse.* → JZComputerUse
//        → JZVirtualDevice(虚拟桌面) → 真实控件状态
//
// 几个刻意的选择:
//   · 后端(虚拟/真实)由**宿主 C++** 决定: 引擎起来**之前** setVirtualDesktop(&desk) 就行,
//     JS 侧一行都不用改 —— 这正是"同一份脚本可确定性回放"的前提;
//   · 感知来源用**布局树本身**(测试里代替 VLM): 建模那一步(认领/建索引)照样走,
//     于是"像素→布局"的认领逻辑也被覆盖, 而不用起任何外部服务;
//   · JS 侧的动作由 **C++ 侧复核**(点完的复选框、输入框里的文字), 两侧对上才算通;
//   · 全程无真实窗口 ⇒ 可无头跑, 已进 ctest。
//
// 注: 本文件只覆盖"虚拟桌面这条链"。真桌面(抓屏/ SendInput / DPI) 不在本测试范围。
#include <cstdio>
#include <string>

#include "JZApplication.h"
#include "JZDebug.h"
#include "JZEventLoop.h"
#include "JZJSEngine.h"
#include "JZJSComputerUseModule.h"
#include "JZTimer.h"
#include "JZVirtualDevice.h"
#include "JZVirtualWindow.h"
#include "JZCheckbox.h"
#include "JZImage.h"
#include "JZImageLabel.h"
#include "JZLabel.h"
#include "JZLineEdit.h"
#include "JZComputerUse/JZComputerUse.h"

#include <quickjs/quickjs.h>

static int g_fail = 0;

static void check(bool ok, const char* msg)
{
    if (ok) {
        printf("  [ ok ] %s\n", msg);
    } else {
        printf("  [FAIL] %s\n", msg);
        ++g_fail;
    }
}

// 求值并取字符串。脚本抛错时 eval 返回的是 JS 异常值, 直接 fromJSValue<std::string> 会崩 ⇒ 先挡掉。
static std::string evalStr(JZJSEngine& engine, const std::string& code)
{
    auto v = engine.eval(code);
    if (engine.isException(v.get()) || JS_IsUndefined(v.get()))
        return std::string();
    return engine.fromJSValue<std::string>(v.get());
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);   // 无缓冲: 崩了也能看到走到哪一步
    JZApplication app;
    JZEventLoop loop;

    // ---- 被测应用: 一个跑在虚拟桌面上的"电源面板" ----
    JZVirtualDevice desk(800, 600, "Virtual Desktop");
    desk.setDesktopBackground(JZColor(232, 232, 232, 255));

    JZVirtualWindow* panel = desk.createWindow(360, 300, "PowerPanel", 120, 90);

    JZCheckbox* power = new JZCheckbox(panel);
    power->setObjectName("btnPower");
    power->setText("Power");
    power->setGeometry(20, 20, 220, 40);
    power->show();

    JZLabel* stateLabel = new JZLabel(panel);
    stateLabel->setObjectName("lblState");
    stateLabel->setText("State: OFF");
    stateLabel->setGeometry(20, 80, 220, 30);
    stateLabel->show();

    JZLineEdit* note = new JZLineEdit(panel);
    note->setObjectName("editNote");
    note->setGeometry(20, 130, 220, 32);
    note->show();

    // 像素族兜底用: 一块**全屏唯一**的颜色块(JS 侧 match({kind:'color'}) 要能命中它)
    JZImage blockImg = JZImage::create(120, 80, JZColor(255, 0, 255, 255));
    JZImageLabel* block = new JZImageLabel(panel);
    block->setObjectName("imgBlock");
    block->setGeometry(20, 180, 120, 80);
    block->setImage(blockImg);
    block->show();

    panel->renderNow();

    // ---- CU 后端切到虚拟桌面(宿主决定后端; 这就是 computeUse->setBackend(JZVirtualDevice)) ----
    JZComputerUse::setVirtualDesktop(&desk);
    check(JZComputerUse::isVirtualDesktop(), "宿主已把 CU 后端切到虚拟桌面");

    // ---- 引擎 + CU 绑定(__jz.computeruse.*) ----
    JZJSEngine engine;
    connect(&engine.sigLog, [](const std::string& s) { jzDebug() << "[js:log] " << s; });
    connect(&engine.sigError, [](const std::string& s) { jzDebug() << "[js:error] " << s; });
    engine.init();
    engine.start();                       // promise/job 泵由宿主事件循环驱动 ⇒ 必须 start
    engine.setLibraryDir({ std::string(JZ_JS_WRAPPER_DIR) });

    JZJSComputerUseModule cuModule;
    cuModule.initEngine(&engine);
    check(!engine.isException(engine.eval("typeof __jz.computeruse === 'object'"
                                          " && typeof __jz.computeruse.layoutTree === 'function'").get()),
          "JS 侧看得见后端 (__jz.computeruse.layoutTree)");

#ifndef JZ_CU_OPENCV
#define JZ_CU_OPENCV 0
#endif

    // ---- 驱动脚本: 就是"一份 CU 脚本会写的样子"(import 包 → 用包的 API 驱动界面) ----
    const std::string wrapper = JZ_JS_WRAPPER_DIR;
    const std::string driverPath = wrapper + "/_test_jscu_driver.mjs";
    const std::string mjs =
        "import * as cu from './computeruse/dist/index.js';\n"
        "globalThis.__cuOpenCv = " + std::string(JZ_CU_OPENCV ? "true" : "false") + ";\n"
        R"JS(
const S = [];
function t(name, ok) { S.push(name + '=' + (ok ? 'ok' : 'FAIL')); }
globalThis.__cuDone = false;
globalThis.__cuErr = '';
globalThis.__cuState = '';

(async () => {
  try {
    // ① 截图: 桌面合成帧(不抓屏)
    const shot = await cu.backend.screenshot();
    t('screenshot', typeof shot === 'string' && shot.indexOf('data:image/png;base64,') === 0);

    // ② 布局通道: 按标题**指定目标窗口**(脚本多半跑在终端里, 靠前台猜必错)
    const layout = await cu.backend.layoutTree({ title: 'PowerPanel' });
    t('layout', !!layout && cu.layoutSource() === 'semantic');

    // ③ 建模: 把"布局树"当感知来源(代替 VLM) → App.build 认领 → attach 建索引。
    //    认领的正是 claimLayout(像素框 → 最贴身的布局节点)那条逻辑。
    const flat = cu.flatten(layout);
    const els = [];
    for (let i = 0; i < flat.length; i++) {
      const e = flat[i];
      if (!e.node.id || !e.node.rect) continue;      // 只认带 viewId 的: 路径才稳定
      els.push({ type: 'label', text: e.node.text || '', rect: e.node.rect });
    }
    const app = cu.App.build({ title: '', elements: els }, { layout });
    const attached = await app.attach(layout, { title: 'PowerPanel' });
    t('attach', attached.bound > 0 && attached.degraded.length === 0 && attached.missing.length === 0);
    t('pageMatch', attached.pageMatch === 1);

    // ④ 布局路径由 viewId 逐级拼出(只有 layout.ts 一份实现, C++ 侧不重复这套规则)
    //    顺便把"根节点与指纹"摊出来: 判页/判弹窗这两条本地分级全建立在它们之上。
    S.push('root=' + (flat[0].node.id || '(no-viewId)') + ':' + flat[0].path
           + ',fp=' + (app.fingerprint ? app.fingerprint.paths.join(',') : '(null)'));
    let btnPath = '', editPath = '';
    for (let i = 0; i < flat.length; i++) {
      if (flat[i].node.id === 'btnPower') btnPath = flat[i].path;
      if (flat[i].node.id === 'editNote') editPath = flat[i].path;
    }
    t('path', btnPath.indexOf('btnPower') > 0 && editPath.indexOf('editNote') > 0);

    // ⑤ 定位 → 点击: 走布局路径(不碰像素)
    const btn = app.at(btnPath);
    const hit = await btn.locate();
    t('locate', !!hit && hit.w > 0 && hit.h > 0);
    await btn.click();
    t('click', true);

    // ⑥ 读状态: 问**当前**布局(不啃 attach 时的快照)
    const st = await app.read(btnPath);
    t('read', st.from === 'layout' && st.checked === true);
    await app.expect(btnPath, { checked: true });      // 断言不满足会抛 expect-failed
    t('expect', true);

    // ⑦ 键盘: 先点输入框拿焦点, 再输入
    await app.at(editPath).click();
    await cu.type('hi');
    const st2 = await app.read(editPath);
    t('type', st2.text === 'hi');

    // ⑧ 稳定判定 + 像素族兜底(颜色)
    t('sync', (await app.sync({ settle: 20, timeoutMs: 2000 })) === true);
    if (globalThis.__cuOpenCv) {
      const col = await cu.backend.match({ kind: 'color', color: '#FF00FF' });
      t('matchColor', !!col && col.w > 0 && col.h > 0);
      t('matchColorMiss', (await cu.backend.match({ kind: 'color', color: '#00FF00' })) === null);
    } else {
      S.push('matchColor=skip');
    }
    // yolo 未实现 ⇒ 明确 null(不假装成功)
    t('yoloUnsupported', (await cu.backend.match({ kind: 'yolo', model: 'x' })) === null);

    // ⑨ 虚拟模式下"启动真实程序"必须**明确失败** —— 绝不误动本机
    t('launchSafe', (await cu.backend.launch({ exe: 'cmd.exe', title: 'PowerPanel' })) === false);

    // ⑩ 期望落空(界面没在期望状态): 本地分级给出结论, 不是静默重试、也不叫模型
    let kind = 'none';
    try {
      await app.expect(btnPath, { checked: false });
    } catch (e) {
      const d = await app.diagnose(e, { attempt: 3, maxAttempts: 3, path: btnPath, expect: { checked: false } });
      kind = d && d.kind ? String(d.kind) : 'none';
      console.log('[CU-test] diagnose -> ' + cu.formatDiagnosis(d));
    }
    S.push('diagnoseKind=' + kind);
    t('diagnose', kind !== 'none' && kind !== '');
  } catch (e) {
    globalThis.__cuErr = String((e && e.message) || e);
  } finally {
    globalThis.__cuState = S.join('|');
    globalThis.__cuDone = true;
  }
})();
)JS";

    FILE* f = fopen(driverPath.c_str(), "wb");
    bool wrote = (f != nullptr);
    if (f) {
        fwrite(mjs.data(), 1, mjs.size(), f);
        fclose(f);
    }
    check(wrote, "驱动脚本已写出");
    check(wrote && engine.runModuleFile(driverPath), "runModuleFile 以 ESM 加载驱动脚本(真的 import 了包)");
    remove(driverPath.c_str());

    // ---- 泵事件循环: 虚拟桌面下注入是同步的, 只要把 promise 的续体(引擎 job)跑完 ----
    auto jsDone = [&]() {
        auto v = engine.eval("globalThis.__cuDone === true");
        return !engine.isException(v.get()) && engine.fromJSValue<bool>(v.get());
    };
    bool finished = false;
    {
        JZTimer poll;
        connect(&poll.sigTimeout, [&]() {
            if (jsDone()) {
                finished = true;
                loop.quit();
            }
        });
        poll.start(20);

        JZTimer wd;                      // 看门狗: 脚本卡住(等一个永远不来的 promise)时别挂死
        wd.setSingleShot(true);
        connect(&wd.sigTimeout, [&]() { loop.quit(); });
        wd.start(60000);

        loop.exec();
        poll.stop();
        wd.stop();
    }
    check(finished, "JS 驱动脚本跑完");

    const std::string jsErr = evalStr(engine, "String(globalThis.__cuErr)");
    if (!jsErr.empty())
        printf("  [js] 脚本异常: %s\n", jsErr.c_str());

    const std::string state = evalStr(engine, "String(globalThis.__cuState)");
    printf("  [js] 结果: %s\n", state.c_str());

    auto has = [&state](const char* key) {
        return state.find(std::string(key) + "=ok") != std::string::npos;
    };

    printf("[JS 侧] 截图 / 布局 / 建模\n");
    check(jsErr.empty(), "脚本没有抛异常");
    check(has("screenshot"), "screenshot 返回 PNG dataURL(虚拟桌面那帧)");
    check(has("layout"), "layoutTree 走语义树 (source=semantic)");
    check(has("attach"), "感知元素全部认领到布局 (无退化项、无遗失)");
    check(has("pageMatch"), "页面指纹与当前布局一致 (pageMatch=1)");
    check(has("path"), "布局路径由 viewId 拼出 (路径稳定)");

    printf("[JS 侧] 定位 / 操作 / 读状态\n");
    check(has("locate"), "按布局路径拿到控件当前几何");
    check(has("click"), "按布局路径点击成功 (不碰像素)");
    check(has("read"), "读状态走布局 (from=layout, checked=true)");
    check(has("expect"), "expect 断言通过");
    check(has("type"), "type 输入落到焦点控件 (editNote=hi)");
    check(has("sync"), "sync 稳定判定通过");

    printf("[JS 侧] 像素族 / 不适用能力\n");
#if JZ_CU_OPENCV
    check(has("matchColor"), "像素族(颜色) 在虚拟桌面上命中");
    check(has("matchColorMiss"), "找不到的颜色返回 null (不假装成功)");
#else
    printf("  [skip] 未接入 OpenCV —— 像素族那条跳过\n");
#endif
    check(has("yoloUnsupported"), "yolo 未实现 ⇒ null (不假装成功)");
    check(has("launchSafe"), "虚拟模式下 launch 明确失败 (不启动真实程序)");

    printf("[JS 侧] 本地分级\n");
    {
        const std::string kind = evalStr(engine, "String(globalThis.__cuState)");
        const std::string tag = "diagnoseKind=";
        const size_t p = kind.find(tag);
        const std::string got = (p == std::string::npos) ? std::string()
                                                         : kind.substr(p + tag.size(),
                                                                       kind.find('|', p) == std::string::npos
                                                                           ? std::string::npos
                                                                           : kind.find('|', p) - (p + tag.size()));
        printf("  [info] 诊断结论: %s\n", got.empty() ? "(空)" : got.c_str());
        check(has("diagnose"), "期望落空时能本地分级 (给出 FailureKind)");
        // 稳定复现(3/3)的状态不符 = 真错 ⇒ precondition/ask-model。
        // ⚠ 曾经这里判成 popup(偶发)/dismiss-popup: 根节点没有 viewId, 指纹里只有带 viewId 的节点,
        //   于是 extraTopPaths 每次把**根**当成"多出来的弹窗" ⇒ 本地分级被一个假弹窗吞掉。
        check(got == "precondition", "稳定复现的状态不符 ⇒ 判 precondition(真错), 不是 popup(偶发)");
    }

    printf("[C++ 复核] JS 的动作真的作用到控件上了吗\n");
    check(power->isChecked(), "复选框真的被勾选 (JS click → 虚拟桌面 → 控件)");
    check(note->text() == "hi", "输入框真的是 hi (JS type → 虚拟桌面 → 焦点控件)");
    check(desk.activeWindow() == panel, "目标窗口被激活");

    // 收尾: 切回真实桌面, 别把虚拟桌面留给后面的代码
    JZComputerUse::setVirtualDesktop(nullptr);
    check(!JZComputerUse::isVirtualDesktop(), "setVirtualDesktop(nullptr) 切回真实桌面");

    printf(g_fail == 0 ? "[JS-CU] 全部通过\n" : "[JS-CU] 失败 %d 项\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
