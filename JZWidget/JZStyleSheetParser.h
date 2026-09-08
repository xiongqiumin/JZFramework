#ifndef JZ_STYLE_SHEET_PARSER_H_
#define JZ_STYLE_SHEET_PARSER_H_

#include <string>
#include <vector>
#include <map>

// CSS 子集解析器 (docs/jzstylesheet-design.md v3.1, M1 子集)。
//
// 职责边界: 只做 "CSS 文本 -> 规则列表"。
//   - 不做属性白名单校验 (属性语义/单位解析属于 JZStyleSet 层, 后续接入);
//   - 不做规则匹配 (resolve/级联, 后续接入);
//   - 变量 (var()) 在 parse 期做文本替换, 作用域仅 :root, 不做级联。
//
// 支持:
//   :root { --name: value; }                   变量定义 (作用域 :root)
//   类型 / .class (多类复合) / #id / * / ::part(name)
//   伪类 :hover :active :checked :disabled :enabled :focus 与 :not(伪类)
//   逗号选择器组 / !important / var(--name) / var(--name, fallback)
//   声明值可为任意文本 (属性合法性交给上层)
// 不支持 (明确留给后续): 后代(空格)与子代(>)等组合器 / @ 规则 / 属性选择器 / 伪元素。
// 遇到上述语法直接 parse 失败并给出 errorString, 不静默丢弃。

// 单条复合选择器 (不含逗号与组合器)
struct JZStyleSelector
{
    std::string type;                      // 类型选择器 (框架类名, 如 JZPushButton); "*" = 通配; 空 = 不限
    std::vector<std::string> classes;      // .class 复合 (全部须匹配)
    std::string id;                        // #objectName; 空 = 不限
    std::vector<std::string> pseudos;      // 状态伪类; :not(x) 记为 "!x"
    std::string part;                      // ::part(name) 部件寻址; 空 = 宿主自身

    bool isRoot() const;                   // 仅 ":root"
    // 特异性 (id, 类/伪类, 类型) 三元组, 按 100/10/1 折算
    int specificity() const;
    std::string toString() const;          // 还原为文本 (报错/调试)
};

struct JZStyleDecl
{
    std::string property;   // 属性名 (已小写化)
    std::string value;      // 值文本 (var() 已替换, !important 已摘除)
    bool important;         // 是否 !important
};

struct JZStyleRule
{
    std::vector<JZStyleSelector> selectors;   // 逗号组展开后的各选择器 (共享同一声明块)
    std::vector<JZStyleDecl> decls;
};

class JZStyleSheetParser
{
public:
    JZStyleSheetParser();
    ~JZStyleSheetParser();

    // 解析整份样式表; 失败返回 false, 原因见 errorString()。
    // 失败时内容保持上一次成功解析的结果 (未修改)。
    bool parse(const std::string& css);
    void clear();

    bool isEmpty() const;
    const std::vector<JZStyleRule>& rules() const;
    // :root 令牌表 (键含前导 "--")
    const std::map<std::string, std::string>& variables() const;

    std::string errorString() const;

private:
    struct Block
    {
        std::string selector;
        std::string body;
    };

    // 预处理: 注释替换为一个空格 (选择器与声明块内都可能出现注释); 未闭合则失败
    bool stripComments(const std::string& css, std::string& out);
    bool splitBlocks(const std::string& css, std::vector<Block>& blocks);
    bool parseSelectorGroup(const std::string& text, std::vector<JZStyleSelector>& out);
    bool parseCompound(const std::string& text, JZStyleSelector& sel);
    // isRoot 为真时只收集变量; 否则只产出普通声明 (vars 传 nullptr)
    bool parseDeclarations(const std::string& body, bool isRoot,
                           std::vector<JZStyleDecl>* decls,
                           std::map<std::string, std::string>* vars);
    bool expandVars(const std::string& value, std::string& out);

    void setError(const std::string& msg);

    std::vector<JZStyleRule> m_rules;
    std::map<std::string, std::string> m_variables;
    std::string m_error;
};

#endif // JZ_STYLE_SHEET_PARSER_H_
