#include "JZStyleSheetParser.h"

#include <ctype.h>
#include <utility>

namespace
{

bool isSpaceChar(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

std::string trimStr(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && isSpaceChar(s[b]))
        b++;
    while (e > b && isSpaceChar(s[e - 1]))
        e--;
    return s.substr(b, e - b);
}

std::string lowerStr(const std::string& s)
{
    std::string r = s;
    for (size_t i = 0; i < r.size(); i++)
    {
        if (r[i] >= 'A' && r[i] <= 'Z')
            r[i] = (char)(r[i] - 'A' + 'a');
    }
    return r;
}

// 标识符字符: 字母/数字/'-'/'_' 及非 ASCII 字节 (如中文字体名)
bool isIdentChar(char c)
{
    unsigned char u = (unsigned char)c;
    return isalnum(u) || c == '-' || c == '_' || u >= 0x80;
}

bool isKnownPseudo(const std::string& p)
{
    return p == "hover" || p == "active" || p == "checked" || p == "disabled"
           || p == "enabled" || p == "focus" || p == "root";
}

} // namespace

// ==================== JZStyleSelector ====================

bool JZStyleSelector::isRoot() const
{
    return type.empty() && classes.empty() && id.empty() && part.empty()
           && pseudos.size() == 1 && pseudos[0] == "root";
}

int JZStyleSelector::specificity() const
{
    int a = id.empty() ? 0 : 1;
    int b = (int)classes.size() + (int)pseudos.size();
    int c = (!type.empty() && type != "*") ? 1 : 0;
    return a * 100 + b * 10 + c;
}

std::string JZStyleSelector::toString() const
{
    std::string s;
    if (!type.empty())
        s += type;
    for (size_t i = 0; i < classes.size(); i++)
        s += "." + classes[i];
    if (!id.empty())
        s += "#" + id;
    if (!part.empty())
        s += "::part(" + part + ")";
    for (size_t i = 0; i < pseudos.size(); i++)
    {
        if (pseudos[i].size() > 0 && pseudos[i][0] == '!')
            s += ":not(:" + pseudos[i].substr(1) + ")";
        else
            s += ":" + pseudos[i];
    }
    return s;
}

// ==================== JZStyleSheetParser ====================

JZStyleSheetParser::JZStyleSheetParser()
{
}

JZStyleSheetParser::~JZStyleSheetParser()
{
}

bool JZStyleSheetParser::isEmpty() const
{
    return m_rules.empty();
}

const std::vector<JZStyleRule>& JZStyleSheetParser::rules() const
{
    return m_rules;
}

const std::map<std::string, std::string>& JZStyleSheetParser::variables() const
{
    return m_variables;
}

std::string JZStyleSheetParser::errorString() const
{
    return m_error;
}

void JZStyleSheetParser::clear()
{
    m_rules.clear();
    m_variables.clear();
    m_error.clear();
}

void JZStyleSheetParser::setError(const std::string& msg)
{
    if (m_error.empty())
        m_error = msg;
}

bool JZStyleSheetParser::parse(const std::string& css)
{
    std::string text;
    if (!stripComments(css, text))
        return false;

    std::vector<Block> blocks;
    if (!splitBlocks(text, blocks))
        return false;

    // 解析成功前不动自身状态: 先在临时对象上构建
    std::vector<JZStyleRule> rules;
    std::map<std::string, std::string> variables;
    std::string savedError = m_error;
    m_error.clear();
    std::swap(m_variables, variables);   // 变量替换需要读已解析的 :root 表

    bool ok = true;

    // 第一遍: 收集 :root 变量 (与书写顺序无关, :root 可出现在任意位置)
    for (size_t i = 0; i < blocks.size() && ok; i++)
    {
        std::vector<JZStyleSelector> sels;
        if (!parseSelectorGroup(blocks[i].selector, sels))
        {
            ok = false;
            break;
        }
        if (sels.size() != 1 || !sels[0].isRoot())
            continue;
        if (!parseDeclarations(blocks[i].body, true, nullptr, &m_variables))
            ok = false;
    }

    // 第二遍: 其余规则 (声明值中的 var() 在此替换)
    for (size_t i = 0; i < blocks.size() && ok; i++)
    {
        std::vector<JZStyleSelector> sels;
        if (!parseSelectorGroup(blocks[i].selector, sels))
        {
            ok = false;
            break;
        }
        if (sels.size() == 1 && sels[0].isRoot())
            continue;

        JZStyleRule rule;
        rule.selectors = sels;
        if (!parseDeclarations(blocks[i].body, false, &rule.decls, nullptr))
        {
            ok = false;
            break;
        }
        if (!rule.decls.empty())
            rules.push_back(rule);
    }

    std::swap(m_variables, variables);
    if (!ok)
    {
        m_error = m_error.empty() ? savedError : m_error;
        return false;
    }

    m_rules = rules;
    m_variables = variables;
    m_error.clear();
    return true;
}

bool JZStyleSheetParser::stripComments(const std::string& css, std::string& out)
{
    out.clear();
    out.reserve(css.size());
    size_t i = 0, n = css.size();
    while (i < n)
    {
        if (css[i] == '/' && i + 1 < n && css[i + 1] == '*')
        {
            size_t end = css.find("*/", i + 2);
            if (end == std::string::npos)
            {
                setError("注释未闭合 (缺少 */)");
                return false;
            }
            out += ' ';
            i = end + 2;
            continue;
        }
        out += css[i];
        i++;
    }
    return true;
}

bool JZStyleSheetParser::splitBlocks(const std::string& css, std::vector<Block>& blocks)
{
    size_t i = 0, n = css.size();
    while (i < n)
    {
        if (isSpaceChar(css[i]))
        {
            i++;
            continue;
        }

        size_t brace = css.find('{', i);
        if (brace == std::string::npos)
        {
            setError("无法解析的片段: " + trimStr(css.substr(i)));
            return false;
        }
        std::string selText = css.substr(i, brace - i);
        if (selText.find('}') != std::string::npos)
        {
            setError("多余的 '}': " + trimStr(selText));
            return false;
        }

        size_t close = css.find('}', brace + 1);
        if (close == std::string::npos)
        {
            setError("声明块未闭合 (缺少 '}'), 选择器: " + trimStr(selText));
            return false;
        }

        Block b;
        b.selector = trimStr(selText);
        b.body = css.substr(brace + 1, close - brace - 1);
        blocks.push_back(b);
        i = close + 1;
    }
    return true;
}

bool JZStyleSheetParser::parseSelectorGroup(const std::string& text, std::vector<JZStyleSelector>& out)
{
    if (text.empty())
    {
        setError("空选择器");
        return false;
    }

    std::vector<std::string> parts;
    std::string cur;
    for (size_t i = 0; i < text.size(); i++)
    {
        if (text[i] == ',')
        {
            parts.push_back(cur);
            cur.clear();
            continue;
        }
        cur += text[i];
    }
    parts.push_back(cur);

    for (size_t i = 0; i < parts.size(); i++)
    {
        std::string s = trimStr(parts[i]);
        if (s.empty())
        {
            setError("选择器组中出现空项: " + text);
            return false;
        }
        if (s.find_first_of(" >+~") != std::string::npos)
        {
            setError("暂不支持组合器 (后代/子代/兄弟): " + s);
            return false;
        }
        JZStyleSelector sel;
        if (!parseCompound(s, sel))
            return false;
        out.push_back(sel);
    }
    return true;
}

bool JZStyleSheetParser::parseCompound(const std::string& s, JZStyleSelector& sel)
{
    size_t i = 0, n = s.size();
    while (i < n)
    {
        char c = s[i];
        if (isSpaceChar(c))
        {
            i++;
            continue;
        }
        if (c == '.')
        {
            i++;
            size_t st = i;
            while (i < n && isIdentChar(s[i]))
                i++;
            if (i == st)
            {
                setError("空类名: " + s);
                return false;
            }
            sel.classes.push_back(s.substr(st, i - st));
        }
        else if (c == '#')
        {
            i++;
            size_t st = i;
            while (i < n && isIdentChar(s[i]))
                i++;
            if (i == st)
            {
                setError("空 id: " + s);
                return false;
            }
            if (!sel.id.empty())
            {
                setError("选择器中 #id 只能出现一次: " + s);
                return false;
            }
            sel.id = s.substr(st, i - st);
        }
        else if (c == ':')
        {
            if (i + 1 < n && s[i + 1] == ':')
            {
                // ::part(name)
                i += 2;
                size_t ks = i;
                while (i < n && isIdentChar(s[i]))
                    i++;
                std::string kw = s.substr(ks, i - ks);
                if (kw != "part")
                {
                    setError("不支持的伪元素 ::" + kw + " (仅支持 ::part)");
                    return false;
                }
                if (i >= n || s[i] != '(')
                {
                    setError("::part 后须跟 (name): " + s);
                    return false;
                }
                i++;
                size_t vs = i;
                while (i < n && s[i] != ')')
                    i++;
                if (i >= n)
                {
                    setError("::part( 未闭合: " + s);
                    return false;
                }
                sel.part = trimStr(s.substr(vs, i - vs));
                i++;
                if (sel.part.empty())
                {
                    setError("::part() 部件名不能为空: " + s);
                    return false;
                }
            }
            else if (i + 5 <= n && s.compare(i + 1, 4, "not(") == 0)
            {
                i += 5;
                size_t vs = i;
                while (i < n && s[i] != ')')
                    i++;
                if (i >= n)
                {
                    setError(":not( 未闭合: " + s);
                    return false;
                }
                std::string inner = trimStr(s.substr(vs, i - vs));
                i++;
                if (inner.empty() || inner[0] != ':')
                {
                    setError(":not() 仅支持单个伪类: " + s);
                    return false;
                }
                inner = trimStr(inner.substr(1));
                if (!isKnownPseudo(inner) || inner == "root")
                {
                    setError("未知或不支持的伪类 :" + inner);
                    return false;
                }
                sel.pseudos.push_back("!" + inner);
            }
            else
            {
                i++;
                size_t st = i;
                while (i < n && isIdentChar(s[i]))
                    i++;
                std::string p = s.substr(st, i - st);
                if (!isKnownPseudo(p))
                {
                    setError("未知伪类 :" + p + " (支持 hover/active/checked/disabled/enabled/focus)");
                    return false;
                }
                sel.pseudos.push_back(p);
            }
        }
        else if (c == '*')
        {
            if (!sel.type.empty())
            {
                setError("选择器中类型只能出现一次: " + s);
                return false;
            }
            sel.type = "*";
            i++;
        }
        else if (isIdentChar(c))
        {
            size_t st = i;
            while (i < n && isIdentChar(s[i]))
                i++;
            if (!sel.type.empty())
            {
                setError("选择器中类型只能出现一次: " + s);
                return false;
            }
            sel.type = s.substr(st, i - st);
        }
        else
        {
            setError(std::string("选择器中出现非法字符 '") + c + "': " + s);
            return false;
        }
    }
    return true;
}

bool JZStyleSheetParser::parseDeclarations(const std::string& body, bool isRoot,
                                           std::vector<JZStyleDecl>* decls,
                                           std::map<std::string, std::string>* vars)
{
    // 按 ';' 切分, 跳过引号内与括号内的分号 (如 font-family: "a;b" / var(--x, a;b))
    std::vector<std::string> parts;
    std::string cur;
    char quote = 0;
    int paren = 0;
    for (size_t i = 0; i < body.size(); i++)
    {
        char c = body[i];
        if (quote)
        {
            cur += c;
            if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '"' || c == '\'')
        {
            quote = c;
            cur += c;
            continue;
        }
        if (c == '(')
        {
            paren++;
            cur += c;
            continue;
        }
        if (c == ')')
        {
            paren--;
            cur += c;
            continue;
        }
        if (c == ';' && paren <= 0)
        {
            parts.push_back(cur);
            cur.clear();
            continue;
        }
        cur += c;
    }
    parts.push_back(cur);   // 末尾允许不写分号

    for (size_t i = 0; i < parts.size(); i++)
    {
        std::string text = trimStr(parts[i]);
        if (text.empty())
            continue;

        size_t colon = text.find(':');
        if (colon == std::string::npos)
        {
            setError("声明缺少 ':' -> " + text);
            return false;
        }
        std::string prop = lowerStr(trimStr(text.substr(0, colon)));
        std::string value = trimStr(text.substr(colon + 1));
        if (prop.empty())
        {
            setError("声明缺少属性名 -> " + text);
            return false;
        }

        bool important = false;
        size_t bang = value.rfind('!');
        if (bang != std::string::npos)
        {
            if (lowerStr(trimStr(value.substr(bang + 1))) == "important")
            {
                important = true;
                value = trimStr(value.substr(0, bang));
            }
        }

        bool isCustom = (prop.size() > 2 && prop[0] == '-' && prop[1] == '-');
        if (isCustom)
        {
            if (!isRoot)
            {
                setError("自定义属性只能定义在 :root -> " + prop);
                return false;
            }
            if (vars)
            {
                std::string v;
                if (!expandVars(value, v))
                    return false;
                (*vars)[prop] = v;
            }
            continue;
        }

        if (isRoot)
        {
            setError(":root 只支持自定义属性 (--name) -> " + prop);
            return false;
        }

        std::string v;
        if (!expandVars(value, v))
            return false;

        JZStyleDecl decl;
        decl.property = prop;
        decl.value = v;
        decl.important = important;
        decls->push_back(decl);
    }
    return true;
}

bool JZStyleSheetParser::expandVars(const std::string& value, std::string& out)
{
    out.clear();
    size_t i = 0;
    while (i < value.size())
    {
        if (i + 4 <= value.size() && value.compare(i, 4, "var(") == 0)
        {
            size_t j = i + 4;
            int depth = 1;
            while (j < value.size())
            {
                if (value[j] == '(')
                    depth++;
                else if (value[j] == ')')
                {
                    depth--;
                    if (depth == 0)
                        break;
                }
                j++;
            }
            if (depth != 0)
            {
                setError("var( 未闭合 -> " + value);
                return false;
            }
            std::string inner = value.substr(i + 4, j - (i + 4));
            i = j + 1;

            // 按第一个顶层逗号切出 fallback
            int d = 0;
            size_t comma = std::string::npos;
            for (size_t k = 0; k < inner.size(); k++)
            {
                if (inner[k] == '(')
                    d++;
                else if (inner[k] == ')')
                    d--;
                else if (inner[k] == ',' && d == 0)
                {
                    comma = k;
                    break;
                }
            }
            std::string name = trimStr(comma == std::string::npos ? inner : inner.substr(0, comma));
            std::string fallback = (comma == std::string::npos) ? std::string() : trimStr(inner.substr(comma + 1));

            if (name.size() < 3 || name[0] != '-' || name[1] != '-')
            {
                setError("var() 参数须为 --name -> " + value);
                return false;
            }

            std::map<std::string, std::string>::const_iterator it = m_variables.find(name);
            if (it != m_variables.end())
            {
                out += it->second;
            }
            else if (!fallback.empty())
            {
                std::string fb;
                if (!expandVars(fallback, fb))
                    return false;
                out += fb;
            }
            else
            {
                setError("未定义的变量 " + name);
                return false;
            }
            continue;
        }
        out += value[i];
        i++;
    }
    return true;
}
