// JZStyleSheet (CSS 子集) 单元测试
//   覆盖: JZStyleSheetParser 解析行为 / JZWidget 控件级样式表与 class / JZApplication 全局样式表
//
// 当前阶段的样式表只落"文本 -> 规则列表 + 素材存取", 尚未接入规则匹配 (resolve) 与生效,
// 因此这里全部是逻辑层断言, 不需要窗口与渲染后端 (JZWidget 未 show 时 update() 直接空转)。
#include "JZTest.h"
#include "JZApplication.h"
#include "JZWidget.h"
#include "JZWindow.h"
#include "JZLabel.h"
#include "JZPushButton.h"
#include "JZStyleSheetParser.h"

#include <string>

namespace
{

const JZStyleRule& rule0(const JZStyleSheetParser& p)
{
    return p.rules().at(0);
}

} // namespace

// ==================== JZStyleSheetParser ====================

static bool parser_basic()
{
    JZStyleSheetParser p;
    JZ_TEST_TRUE(p.parse("/* 注释 */ JZPushButton { background-color: #e0e0e0; border: 1px solid #808080; }"));
    JZ_TEST_EQ(1u, p.rules().size());

    const JZStyleRule& r = rule0(p);
    JZ_TEST_EQ(1u, r.selectors.size());
    JZ_TEST_EQ(std::string("JZPushButton"), r.selectors[0].type);
    JZ_TEST_EQ(2u, r.decls.size());
    JZ_TEST_EQ(std::string("background-color"), r.decls[0].property);
    JZ_TEST_EQ(std::string("#e0e0e0"), r.decls[0].value);
    JZ_TEST_FALSE(r.decls[0].important);
    JZ_TEST_EQ(std::string("border"), r.decls[1].property);
    JZ_TEST_EQ(std::string("1px solid #808080"), r.decls[1].value);   // 简写值原样保留
    return true;
}

static bool parser_decls()
{
    JZStyleSheetParser p;
    // 属性名大小写不敏感 + 末尾声明可省略分号 + 多值简写
    JZ_TEST_TRUE(p.parse(".btn { PADDING: 7px 14px 8px; Color: #202020; border-radius: 8px }"));
    const JZStyleRule& r = rule0(p);
    JZ_TEST_EQ(3u, r.decls.size());
    JZ_TEST_EQ(std::string("padding"), r.decls[0].property);
    JZ_TEST_EQ(std::string("7px 14px 8px"), r.decls[0].value);
    JZ_TEST_EQ(std::string("color"), r.decls[1].property);
    JZ_TEST_EQ(std::string("border-radius"), r.decls[2].property);
    JZ_TEST_EQ(std::string("8px"), r.decls[2].value);
    return true;
}

static bool parser_important()
{
    JZStyleSheetParser p;
    JZ_TEST_TRUE(p.parse(".a { color: red !important; background: none; }"));
    JZ_TEST_TRUE(rule0(p).decls[0].important);
    JZ_TEST_EQ(std::string("red"), rule0(p).decls[0].value);   // !important 已摘除
    JZ_TEST_FALSE(rule0(p).decls[1].important);
    return true;
}

static bool parser_selector_forms()
{
    JZStyleSheetParser p;
    JZ_TEST_TRUE(p.parse(".btn.primary#ok::part(arrow):hover:not(:disabled) { color: #fff; }"));
    const JZStyleSelector& s = rule0(p).selectors[0];

    JZ_TEST_TRUE(s.type.empty());
    JZ_TEST_EQ(2u, s.classes.size());
    JZ_TEST_EQ(std::string("btn"), s.classes[0]);
    JZ_TEST_EQ(std::string("primary"), s.classes[1]);
    JZ_TEST_EQ(std::string("ok"), s.id);
    JZ_TEST_EQ(std::string("arrow"), s.part);
    JZ_TEST_EQ(2u, s.pseudos.size());
    JZ_TEST_EQ(std::string("hover"), s.pseudos[0]);
    JZ_TEST_EQ(std::string("!disabled"), s.pseudos[1]);   // :not(x) 记为 "!x"
    return true;
}

static bool parser_selector_group()
{
    JZStyleSheetParser p;
    JZ_TEST_TRUE(p.parse("JZPushButton, .btn, * { color: #202020; }"));
    const JZStyleRule& r = rule0(p);
    JZ_TEST_EQ(3u, r.selectors.size());          // 逗号组展开, 共享同一声明块
    JZ_TEST_EQ(std::string("JZPushButton"), r.selectors[0].type);
    JZ_TEST_EQ(std::string("btn"), r.selectors[1].classes[0]);
    JZ_TEST_EQ(std::string("*"), r.selectors[2].type);
    JZ_TEST_EQ(1u, r.decls.size());
    return true;
}

static bool parser_specificity()
{
    JZStyleSheetParser p;
    JZ_TEST_TRUE(p.parse("JZPushButton { color: red; }"));           // 类型 = 1
    JZ_TEST_EQ(1, rule0(p).selectors[0].specificity());

    JZ_TEST_TRUE(p.parse(".btn { color: red; }"));                   // 类 = 10
    JZ_TEST_EQ(10, rule0(p).selectors[0].specificity());

    JZ_TEST_TRUE(p.parse("#ok { color: red; }"));                    // id = 100
    JZ_TEST_EQ(100, rule0(p).selectors[0].specificity());

    JZ_TEST_TRUE(p.parse(".btn.primary:active { color: red; }"));    // 类*2 + 伪类 = 30
    JZ_TEST_EQ(30, rule0(p).selectors[0].specificity());

    JZ_TEST_TRUE(p.parse(".btn.primary#ok::part(arrow):hover:not(:disabled) { color: red; }"));
    JZ_TEST_EQ(140, rule0(p).selectors[0].specificity());            // 100 + 40 + 0

    JZ_TEST_TRUE(p.parse("* { color: red; }"));                      // 通配不计特异性
    JZ_TEST_EQ(0, rule0(p).selectors[0].specificity());
    return true;
}

// 空声明块的规则没有意义, 解析后直接丢弃 (不进 rules)
static bool parser_empty_block_dropped()
{
    JZStyleSheetParser p;
    JZ_TEST_TRUE(p.parse("JZPushButton { } .btn { color: red; }"));
    JZ_TEST_EQ(1u, p.rules().size());
    JZ_TEST_EQ(std::string("btn"), rule0(p).selectors[0].classes[0]);

    JZ_TEST_TRUE(p.parse("JZPushButton { /* 只有注释 */ }"));
    JZ_TEST_TRUE(p.isEmpty());
    return true;
}

static bool parser_selector_toString()
{
    JZStyleSheetParser p;
    JZ_TEST_TRUE(p.parse(".btn.primary#ok::part(arrow):hover:not(:disabled) { color: red; }"));
    JZ_TEST_EQ(std::string(".btn.primary#ok::part(arrow):hover:not(:disabled)"),
               rule0(p).selectors[0].toString());
    return true;
}

static bool parser_variables()
{
    // :root 写在后面也能生效 (两遍解析), 与 CSS 书写顺序无关
    JZStyleSheetParser p;
    JZ_TEST_TRUE(p.parse("JZPushButton { background-color: var(--bg); }\n"
                         ":root { --bg: #3498db; --text: #202020; }"));
    JZ_TEST_EQ(2u, p.variables().size());
    JZ_TEST_EQ(std::string("#3498db"), p.variables().find("--bg")->second);
    JZ_TEST_EQ(std::string("#3498db"), rule0(p).decls[0].value);
    JZ_TEST_TRUE(p.isEmpty() == false);

    // :root 规则本身不进 rules
    JZ_TEST_EQ(1u, p.rules().size());
    return true;
}

static bool parser_variables_fallback()
{
    JZStyleSheetParser p;
    JZ_TEST_TRUE(p.parse(":root { --bg: #3498db; }\n"
                         ".a { padding: var(--pad, 4px); border-color: var(--none, rgb(1, 2, 3)); }"));
    const JZStyleRule& r = rule0(p);
    JZ_TEST_EQ(std::string("4px"), r.decls[0].value);                    // fallback 生效
    JZ_TEST_EQ(std::string("rgb(1, 2, 3)"), r.decls[1].value);           // fallback 含逗号/括号
    return true;
}

static bool parser_errors()
{
    // 全部应解析失败, 且不静默丢弃 (errorString 有内容)
    const char* bad[] = {
        "JZPushButton { color red; }",              // 缺少 ':'
        "JZPushButton { color: red; ",              // 声明块未闭合
        "JZPushButton { color: red; } .a ",         // 尾部残留片段
        "JZPushButton { } }",                       // 多余 '}'
        ".a .b { color: red; }",                    // 后代组合器 (暂不支持)
        ".a > .b { color: red; }",                  // 子代组合器
        "JZPushButton::before { color: red; }",     // 伪元素 (仅支持 ::part)
        "JZPushButton:visited { color: red; }",     // 未知伪类
        "JZPushButton::part() { color: red; }",     // 空部件名
        ".a { --x: red; }",                         // 非 :root 定义变量
        ":root { color: red; }",                    // :root 写普通属性
        "JZPushButton { color: var(--nope); }",     // 未定义变量且无 fallback
        "JZPushButton { color: var(--nope",         // var( 未闭合
        "/* 未闭合",                                // 注释未闭合
        "{ color: red; }",                          // 空选择器
    };
    for (int i = 0; i < (int)(sizeof(bad) / sizeof(bad[0])); i++)
    {
        JZStyleSheetParser p;
        JZ_TEST_FALSE(p.parse(bad[i]));
        JZ_TEST_TRUE(!p.errorString().empty());
    }
    return true;
}

static bool parser_failure_keeps_previous()
{
    JZStyleSheetParser p;
    JZ_TEST_TRUE(p.parse(".a { color: red; }"));
    JZ_TEST_FALSE(p.parse(".b { color: red"));
    JZ_TEST_EQ(1u, p.rules().size());                              // 保留上次成功的结果
    JZ_TEST_EQ(std::string("a"), rule0(p).selectors[0].classes[0]);
    JZ_TEST_TRUE(!p.errorString().empty());

    // 成功解析后 errorString 清空
    JZ_TEST_TRUE(p.parse(".c { color: blue; }"));
    JZ_TEST_TRUE(p.errorString().empty());
    JZ_TEST_EQ(std::string("c"), rule0(p).selectors[0].classes[0]);
    return true;
}

static bool parser_empty_and_clear()
{
    JZStyleSheetParser p;
    JZ_TEST_TRUE(p.parse(""));
    JZ_TEST_TRUE(p.isEmpty());
    JZ_TEST_TRUE(p.variables().empty());

    JZ_TEST_TRUE(p.parse(":root { --a: 1px; } .x { color: red; }"));
    JZ_TEST_FALSE(p.isEmpty());
    p.clear();
    JZ_TEST_TRUE(p.isEmpty());
    JZ_TEST_TRUE(p.variables().empty());
    return true;
}

// ==================== JZWidget 样式表 API ====================

static bool widget_style_class()
{
    JZWidget w;
    JZ_TEST_TRUE(w.styleClass().empty());
    w.setStyleClass("btn primary");
    JZ_TEST_EQ(std::string("btn primary"), w.styleClass());
    w.setStyleClass("btn");
    JZ_TEST_EQ(std::string("btn"), w.styleClass());
    return true;
}

static bool widget_style_sheet()
{
    JZWidget w;
    JZ_TEST_TRUE(w.styleSheet().empty());
    JZ_TEST_TRUE(w.styleSheetParser() == nullptr);        // 未设置过: 懒创建前为 nullptr

    JZ_TEST_TRUE(w.setStyleSheet("JZPushButton { color: red; }"));
    JZ_TEST_EQ(std::string("JZPushButton { color: red; }"), w.styleSheet());
    JZ_TEST_TRUE(w.styleSheetParser() != nullptr);
    JZ_TEST_EQ(1u, w.styleSheetParser()->rules().size());
    JZ_TEST_TRUE(w.styleSheetError().empty());
    return true;
}

static bool widget_style_sheet_error()
{
    JZWidget w;
    JZ_TEST_TRUE(w.setStyleSheet(".a { color: red; }"));

    JZ_TEST_FALSE(w.setStyleSheet(".b { color: red"));    // 语法错误
    JZ_TEST_TRUE(!w.styleSheetError().empty());
    JZ_TEST_EQ(std::string(".a { color: red; }"), w.styleSheet());      // 旧文本保留
    JZ_TEST_EQ(std::string("a"), w.styleSheetParser()->rules().at(0).selectors[0].classes[0]);
    return true;
}

// ==================== JZApplication 全局样式表 ====================

static bool app_style_sheet()
{
    JZApplication* app = JZApplication::instance();
    JZ_TEST_TRUE(app != nullptr);

    JZ_TEST_TRUE(app->setStyleSheet(":root { --accent: #3498db; } .btn { color: var(--accent); }"));
    JZ_TEST_EQ(std::string(":root { --accent: #3498db; } .btn { color: var(--accent); }"), app->styleSheet());
    JZ_TEST_TRUE(app->styleSheetParser() != nullptr);
    JZ_TEST_EQ(1u, app->styleSheetParser()->rules().size());
    JZ_TEST_EQ(std::string("#3498db"), app->styleSheetParser()->rules().at(0).decls[0].value);
    return true;
}

static bool app_style_sheet_error()
{
    JZApplication* app = JZApplication::instance();
    JZ_TEST_TRUE(app != nullptr);

    JZ_TEST_FALSE(app->setStyleSheet(".x { color: red"));
    JZ_TEST_TRUE(!app->styleSheetError().empty());
    JZ_TEST_TRUE(app->styleSheet().find(".btn") != std::string::npos);   // 旧文本保留
    return true;
}

// ==================== 样式解析 (resolve) ====================

static bool resolve_ua_default()
{
    JZApplication::instance()->setStyleSheet("");   // 无 author 表: 应完全等于 UA 默认

    JZPushButton b;
    const JZStyleSet& s = b.resolvedStyle();
    JZ_TEST_TRUE(s.background.isSome());
    JZ_TEST_EQ(0xe0, (int)s.background.value().red());   // #e0e0e0
    JZ_TEST_EQ(4, s.radius);                             // 4px 圆角
    JZ_TEST_TRUE(s.color.isSome());
    JZ_TEST_EQ(0x20, (int)s.color.value().red());        // #202020
    JZ_TEST_TRUE(s.borderColor.isSome());
    JZ_TEST_EQ(1, s.borderWidth);

    JZLabel l;
    JZ_TEST_EQ(0x33, (int)l.resolvedStyle().color.value().red());   // #333333
    return true;
}

static bool resolve_global_sheet()
{
    JZ_TEST_TRUE(jzApp->setStyleSheet("JZPushButton { background-color: #3498db; border-radius: 10px; }"));

    JZPushButton b;
    const JZStyleSet& s = b.resolvedStyle();
    JZ_TEST_EQ(0x34, (int)s.background.value().red());   // author 覆盖 UA
    JZ_TEST_EQ(10, s.radius);
    JZ_TEST_EQ(0x20, (int)s.color.value().red());        // 未写的属性回落 UA 默认
    return true;
}

static bool resolve_specificity()
{
    JZ_TEST_TRUE(jzApp->setStyleSheet("JZLabel { color: #333333; } .title { color: #3498db; }"));

    JZLabel l1;
    JZ_TEST_EQ(0x33, (int)l1.resolvedStyle().color.value().red());

    JZLabel l2;
    l2.setStyleClass("title");
    JZ_TEST_EQ(0x34, (int)l2.resolvedStyle().color.value().red());   // 类(10) > 类型(1)
    return true;
}

static bool resolve_important()
{
    JZ_TEST_TRUE(jzApp->setStyleSheet(".a { color: #111111; } .b { color: #222222 !important; }"));

    JZWidget w;
    w.setStyleClass("a b");
    JZ_TEST_EQ(0x22, (int)w.resolvedStyle().color.value().red());    // !important 最后应用
    return true;
}

static bool resolve_widget_sheet_wins()
{
    JZ_TEST_TRUE(jzApp->setStyleSheet("JZPushButton { background-color: #111111; }"));

    JZPushButton b;
    JZ_TEST_TRUE(b.setStyleSheet("JZPushButton { background-color: #999999; }"));
    JZ_TEST_EQ(0x99, (int)b.resolvedStyle().background.value().red());   // 控件级表优先于全局表
    return true;
}

// 换肤核心: 组件规则一字不改, 只换 :root 令牌块 (整表替换)
static bool resolve_token_swap()
{
    const char* rules = "JZPushButton { background-color: var(--accent); }";

    JZ_TEST_TRUE(jzApp->setStyleSheet(std::string(":root { --accent: #3498db; }") + rules));
    JZPushButton b1;
    JZ_TEST_EQ(0x34, (int)b1.resolvedStyle().background.value().red());

    JZ_TEST_TRUE(jzApp->setStyleSheet(std::string(":root { --accent: #4f8cff; }") + rules));
    JZPushButton b2;
    JZ_TEST_EQ(0x4f, (int)b2.resolvedStyle().background.value().red());
    return true;
}

// 换表后窗口整树自动失效 (缓存必须重算)
static bool resolve_invalidate_tree()
{
    JZ_TEST_TRUE(jzApp->setStyleSheet("JZPushButton { background-color: #111111; }"));

    JZWindow win;
    JZPushButton* b = new JZPushButton(&win);
    JZ_TEST_EQ(0x11, (int)b->resolvedStyle().background.value().red());

    JZ_TEST_TRUE(jzApp->setStyleSheet("JZPushButton { background-color: #222222; }"));
    JZ_TEST_EQ(0x22, (int)b->resolvedStyle().background.value().red());   // 整树 styleChanged 生效

    // 类变化也会让自身缓存失效
    JZ_TEST_TRUE(jzApp->setStyleSheet(".hot { background-color: #e74c3c; }"));
    b->setStyleClass("hot");
    JZ_TEST_EQ(0xe7, (int)b->resolvedStyle().background.value().red());
    return true;
}

// JZWidget 基类: UA 默认无外观 (不画背景/边框, 露出父控件背景)
static bool resolve_base_widget_ua()
{
    jzApp->setStyleSheet("");
    JZWidget w;
    const JZStyleSet& s = w.resolvedStyle();
    JZ_TEST_TRUE(s.background.isNone());
    JZ_TEST_TRUE(s.borderColor.isNone());
    JZ_TEST_TRUE(s.color.isNone());
    JZ_TEST_EQ(0, s.radius);
    JZ_TEST_EQ(0, s.paddingLeft);

    // 给了样式才画: 通用容器因此可用 CSS 设背景/边框/圆角
    JZ_TEST_TRUE(jzApp->setStyleSheet(".panel { background-color: #f5f7fa; border: 1px solid #d4dbe6; border-radius: 6px; }"));
    JZWidget w2;
    w2.setStyleClass("panel");
    const JZStyleSet& s2 = w2.resolvedStyle();
    JZ_TEST_TRUE(s2.background.isSome());
    JZ_TEST_EQ(0xf5, (int)s2.background.value().red());
    JZ_TEST_EQ(0xd4, (int)s2.borderColor.value().red());
    JZ_TEST_EQ(6, s2.radius);
    return true;
}

// JZLabel: 硬编码的"左右 6px"已显式化为 padding (无样式表时排版不变, 且可被覆盖)
static bool resolve_label_padding()
{
    jzApp->setStyleSheet("");
    JZLabel l;
    JZ_TEST_EQ(6, l.resolvedStyle().paddingLeft);
    JZ_TEST_EQ(6, l.resolvedStyle().paddingRight);
    JZ_TEST_EQ(0x33, (int)l.resolvedStyle().color.value().red());

    // 2 值 = 上下 / 左右
    JZ_TEST_TRUE(jzApp->setStyleSheet("JZLabel { padding: 0 16px; color: #e6e9ef; }"));
    JZLabel l2;
    JZ_TEST_EQ(0, l2.resolvedStyle().paddingTop);
    JZ_TEST_EQ(16, l2.resolvedStyle().paddingLeft);
    JZ_TEST_EQ(16, l2.resolvedStyle().paddingRight);
    JZ_TEST_EQ(0xe6, (int)l2.resolvedStyle().color.value().red());

    // 4 值 = 上 / 右 / 下 / 左
    JZ_TEST_TRUE(jzApp->setStyleSheet("JZLabel { padding: 1px 2px 3px 4px; }"));
    JZLabel l3;
    JZ_TEST_EQ(1, l3.resolvedStyle().paddingTop);
    JZ_TEST_EQ(2, l3.resolvedStyle().paddingRight);
    JZ_TEST_EQ(3, l3.resolvedStyle().paddingBottom);
    JZ_TEST_EQ(4, l3.resolvedStyle().paddingLeft);
    return true;
}

// JZPushButton: 盒子属性全部可被样式表覆盖, 未写的回落 UA
static bool resolve_button_box()
{
    JZ_TEST_TRUE(jzApp->setStyleSheet(
        "JZPushButton { background-color: #2a3140; border: 2px solid #4a5364; "
        "border-radius: 20px; color: #e6e9ef; padding: 8px 16px; }"));

    JZPushButton b;
    const JZStyleSet& s = b.resolvedStyle();
    JZ_TEST_EQ(0x2a, (int)s.background.value().red());
    JZ_TEST_EQ(0x4a, (int)s.borderColor.value().red());
    JZ_TEST_EQ(2, s.borderWidth);
    JZ_TEST_EQ(20, s.radius);
    JZ_TEST_EQ(0xe6, (int)s.color.value().red());
    JZ_TEST_EQ(8, s.paddingTop);
    JZ_TEST_EQ(16, s.paddingLeft);
    return true;
}

static void test_stylesheet(JZTest* test)
{
    auto parserSuite = std::make_shared<JZTestSuite>("JZStyleSheetParser");
    test->addSuite(parserSuite);
    parserSuite->addTestCase("basic", parser_basic);
    parserSuite->addTestCase("decls", parser_decls);
    parserSuite->addTestCase("important", parser_important);
    parserSuite->addTestCase("selector_forms", parser_selector_forms);
    parserSuite->addTestCase("selector_group", parser_selector_group);
    parserSuite->addTestCase("specificity", parser_specificity);
    parserSuite->addTestCase("empty_block_dropped", parser_empty_block_dropped);
    parserSuite->addTestCase("selector_toString", parser_selector_toString);
    parserSuite->addTestCase("variables", parser_variables);
    parserSuite->addTestCase("variables_fallback", parser_variables_fallback);
    parserSuite->addTestCase("errors", parser_errors);
    parserSuite->addTestCase("failure_keeps_previous", parser_failure_keeps_previous);
    parserSuite->addTestCase("empty_and_clear", parser_empty_and_clear);

    auto widgetSuite = std::make_shared<JZTestSuite>("JZWidgetStyleApi");
    test->addSuite(widgetSuite);
    widgetSuite->addTestCase("style_class", widget_style_class);
    widgetSuite->addTestCase("style_sheet", widget_style_sheet);
    widgetSuite->addTestCase("style_sheet_error", widget_style_sheet_error);

    auto resolveSuite = std::make_shared<JZTestSuite>("JZStyleResolve");
    test->addSuite(resolveSuite);
    resolveSuite->addTestCase("ua_default", resolve_ua_default);
    resolveSuite->addTestCase("global_sheet", resolve_global_sheet);
    resolveSuite->addTestCase("specificity", resolve_specificity);
    resolveSuite->addTestCase("important", resolve_important);
    resolveSuite->addTestCase("widget_sheet_wins", resolve_widget_sheet_wins);
    resolveSuite->addTestCase("token_swap", resolve_token_swap);
    resolveSuite->addTestCase("invalidate_tree", resolve_invalidate_tree);
    resolveSuite->addTestCase("base_widget_ua", resolve_base_widget_ua);
    resolveSuite->addTestCase("label_padding", resolve_label_padding);
    resolveSuite->addTestCase("button_box", resolve_button_box);

    auto appSuite = std::make_shared<JZTestSuite>("JZApplicationStyleApi");
    test->addSuite(appSuite);
    appSuite->addTestCase("style_sheet", app_style_sheet);
    appSuite->addTestCase("style_sheet_error", app_style_sheet_error);
}

int main(int argc, char* argv[])
{
    JZApplication app;   // 单例: JZWidget 与样式表 API 的宿主

    JZTest test;
    test_stylesheet(&test);
    return test.run(argc, argv);
}
