#include "JZXml.h"

#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <sstream>

// ============================================================================
// 内部辅助
// ============================================================================
namespace
{

inline bool isSpaceChar(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

inline bool isNameStart(char c)
{
    unsigned char u = (unsigned char)c;
    return std::isalpha(u) || c == '_' || c == ':' || u >= 0x80;
}

inline bool isNameChar(char c)
{
    unsigned char u = (unsigned char)c;
    return isNameStart(c) || std::isdigit(u) || c == '-' || c == '.';
}

// 代码点 -> UTF-8
void appendUtf8(std::string& out, int code)
{
    if (code < 0x80)
    {
        out += (char)code;
    }
    else if (code < 0x800)
    {
        out += (char)(0xC0 | (code >> 6));
        out += (char)(0x80 | (code & 0x3F));
    }
    else if (code < 0x10000)
    {
        out += (char)(0xE0 | (code >> 12));
        out += (char)(0x80 | ((code >> 6) & 0x3F));
        out += (char)(0x80 | (code & 0x3F));
    }
    else
    {
        out += (char)(0xF0 | (code >> 18));
        out += (char)(0x80 | ((code >> 12) & 0x3F));
        out += (char)(0x80 | ((code >> 6) & 0x3F));
        out += (char)(0x80 | (code & 0x3F));
    }
}

// 文本 / 属性值中的实体解码
std::string decodeEntities(const std::string& s)
{
    std::string out;
    size_t i = 0, n = s.size();
    while (i < n)
    {
        if (s[i] == '&')
        {
            size_t j = s.find(';', i + 1);
            if (j != std::string::npos)
            {
                std::string token = s.substr(i + 1, j - i - 1);
                if (token == "amp") out += '&';
                else if (token == "lt") out += '<';
                else if (token == "gt") out += '>';
                else if (token == "quot") out += '"';
                else if (token == "apos") out += '\'';
                else if (!token.empty() && token[0] == '#')
                {
                    int code = 0;
                    if (token.size() > 1 && (token[1] == 'x' || token[1] == 'X'))
                        code = (int)std::strtol(token.c_str() + 2, nullptr, 16);
                    else
                        code = (int)std::strtol(token.c_str() + 1, nullptr, 10);
                    if (code > 0)
                        appendUtf8(out, code);
                }
                else
                {
                    // 未知实体: 原样保留
                    out += s.substr(i, j - i + 1);
                }
                i = j + 1;
                continue;
            }
            // 找不到 ';' 的裸 '&' 原样保留
            out += '&';
            i++;
        }
        else
        {
            out += s[i];
            i++;
        }
    }
    return out;
}

void escapeText(std::string& out, const std::string& s)
{
    for (char c : s)
    {
        switch (c)
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        default: out += c; break;
        }
    }
}

void escapeAttr(std::string& out, const std::string& s)
{
    for (char c : s)
    {
        switch (c)
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out += c; break;
        }
    }
}

void removeFromVector(std::vector<JZDomNodeDataPtr>& vec, const JZDomNodeDataPtr& node)
{
    auto it = std::find(vec.begin(), vec.end(), node);
    if (it != vec.end())
        vec.erase(it);
}

// 前向声明 (定义见文件末尾)
static std::string str_trim(const std::string& s);

// ============================================================================
// 序列化
// ============================================================================
void writeNodeInline(std::string& out, const JZDomNodeDataPtr& node)
{
    switch (node->type)
    {
    case JZDomNodeData::Type::Text:
    case JZDomNodeData::Type::CDATASection:
    case JZDomNodeData::Type::Comment:
        if (node->type == JZDomNodeData::Type::Text) escapeText(out, node->value);
        else if (node->type == JZDomNodeData::Type::CDATASection) out += "<![CDATA[" + node->value + "]]>";
        else out += "<!--" + node->value + "-->";
        break;
    case JZDomNodeData::Type::ProcessingInstruction:
        out += "<?" + node->name;
        if (!node->value.empty()) out += " " + node->value;
        out += "?>";
        break;
    case JZDomNodeData::Type::DocumentType:
    {
        out += "<!DOCTYPE " + node->name;
        if (!node->publicId.empty()) out += " PUBLIC \"" + node->publicId + "\"";
        if (!node->systemId.empty())
            out += (node->publicId.empty() ? " SYSTEM" : "") + std::string(" \"") + node->systemId + "\"";
        out += ">";
        break;
    }
    default:
        break;
    }
}

void writeElement(std::string& out, const JZDomNodeDataPtr& el, int level, int indent)
{
    std::string pad = (indent > 0) ? std::string(level * indent, ' ') : "";
    std::string attrs;
    for (const auto& a : el->attributes)
    {
        attrs += " ";
        attrs += a->name;
        attrs += "=\"";
        escapeAttr(attrs, a->value);
        attrs += "\"";
    }

    bool hasElementChild = false;
    for (const auto& c : el->children)
        if (c->type == JZDomNodeData::Type::Element) { hasElementChild = true; break; }

    if (el->children.empty())
    {
        out += pad + "<" + el->name + attrs + "/>";
        if (indent > 0) out += "\n";
        return;
    }

    if (!hasElementChild)
    {
        out += pad + "<" + el->name + attrs + ">";
        for (const auto& c : el->children)
            writeNodeInline(out, c);
        out += "</" + el->name + ">";
        if (indent > 0) out += "\n";
        return;
    }

    // 含元素子节点: 美化输出
    out += pad + "<" + el->name + attrs + ">";
    if (indent > 0) out += "\n";
    std::string childPad = (indent > 0) ? std::string((level + 1) * indent, ' ') : "";
    for (const auto& c : el->children)
    {
        if (c->type == JZDomNodeData::Type::Element)
        {
            writeElement(out, c, level + 1, indent);
        }
        else
        {
            out += childPad;
            writeNodeInline(out, c);
            if (indent > 0) out += "\n";
        }
    }
    out += pad + "</" + el->name + ">";
    if (indent > 0) out += "\n";
}

std::string writeNodeToString(const JZDomNodeDataPtr& node, int indent)
{
    std::string out;
    if (!node)
        return out;
    if (node->type == JZDomNodeData::Type::Document)
    {
        for (const auto& c : node->children)
        {
            if (c->type == JZDomNodeData::Type::Element)
                writeElement(out, c, 0, indent);
            else
            {
                std::string pad = (indent > 0) ? std::string(indent, ' ') : "";
                out += pad;
                writeNodeInline(out, c);
                if (indent > 0) out += "\n";
            }
        }
        return out;
    }
    if (node->type == JZDomNodeData::Type::Element)
        writeElement(out, node, 0, indent);
    else
        writeNodeInline(out, node);
    return out;
}

// ============================================================================
// DOM 解析器
// ============================================================================
class JZXmlParser
{
public:
    explicit JZXmlParser(const std::string& text)
        : m_text(text), m_pos(0) {}

    bool parse(JZDomNodeDataPtr& outDoc, std::string* errorMsg, int* errorLine, int* errorColumn)
    {
        m_doc = std::make_shared<JZDomNodeData>();
        m_doc->type = JZDomNodeData::Type::Document;

        skipSpace();
        parseProlog();
        if (!m_error.empty()) return fail(errorMsg, errorLine, errorColumn);

        // 根元素
        if (peek() == '<' && peek(1) != '/')
        {
            JZDomNodeDataPtr root = parseElement();
            if (!root) return fail(errorMsg, errorLine, errorColumn);
            m_doc->children.push_back(root);
            root->parent = m_doc.get();
        }

        // 尾部注释 / 处理指令 / 空白
        while (!atEnd())
        {
            skipSpace();
            if (atEnd()) break;
            if (peek() == '<' && peek(1) == '?')
            {
                JZDomNodeDataPtr pi = parsePI();
                if (!pi) return fail(errorMsg, errorLine, errorColumn);
                pi->parent = m_doc.get();
                m_doc->children.push_back(pi);
            }
            else if (peek() == '<' && peek(1) == '!' && peek(2) == '-' && peek(3) == '-')
            {
                JZDomNodeDataPtr c = parseComment();
                if (!c) return fail(errorMsg, errorLine, errorColumn);
                c->parent = m_doc.get();
                m_doc->children.push_back(c);
            }
            else if (peek() == '<' && peek(1) != '/')
            {
                setError("文档存在多个根元素");
                return fail(errorMsg, errorLine, errorColumn);
            }
            else
            {
                setError("文档尾部存在非法内容");
                return fail(errorMsg, errorLine, errorColumn);
            }
        }

        if (!m_doc->children.empty() && m_doc->children.back()->type == JZDomNodeData::Type::Element)
        {
            // ok
        }
        outDoc = m_doc;
        return true;
    }

private:
    const std::string& m_text;
    size_t m_pos;
    JZDomNodeDataPtr m_doc;
    std::string m_error;

    bool atEnd() const { return m_pos >= m_text.size(); }
    char peek(int off = 0) const
    {
        if (m_pos + off >= m_text.size()) return 0;
        return m_text[m_pos + off];
    }
    char next()
    {
        if (atEnd()) return 0;
        return m_text[m_pos++];
    }
    void skipSpace()
    {
        while (!atEnd() && isSpaceChar(peek()))
            m_pos++;
    }
    bool tryMatch(const std::string& s)
    {
        if (m_text.compare(m_pos, s.size(), s) == 0)
        {
            m_pos += s.size();
            return true;
        }
        return false;
    }

    void setError(const std::string& msg)
    {
        int line = 1, col = 1;
        for (size_t i = 0; i < m_pos; i++)
        {
            if (m_text[i] == '\n') { line++; col = 1; }
            else col++;
        }
        m_error = msg + " (行 " + std::to_string(line) + ", 列 " + std::to_string(col) + ")";
    }

    bool fail(std::string* errorMsg, int* errorLine, int* errorColumn)
    {
        if (errorMsg) *errorMsg = m_error;
        if (errorLine || errorColumn)
        {
            int line = 1, col = 1;
            for (size_t i = 0; i < m_pos; i++)
            {
                if (m_text[i] == '\n') { line++; col = 1; }
                else col++;
            }
            if (errorLine) *errorLine = line;
            if (errorColumn) *errorColumn = col;
        }
        return false;
    }

    std::string readName()
    {
        std::string name;
        if (!isNameStart(peek()))
        {
            setError("非法的元素 / 属性名");
            return name;
        }
        while (!atEnd() && isNameChar(peek()))
            name += next();
        return name;
    }

    // 读取一段直到 endChars 中任一字符 (不含), 处理实体
    std::string readUntil(const std::string& endChars)
    {
        std::string s;
        while (!atEnd())
        {
            char c = peek();
            if (endChars.find(c) != std::string::npos)
                break;
            s += next();
        }
        return s;
    }

    void parseProlog()
    {
        // XML 声明
        if (peek() == '<' && peek(1) == '?' && peek(2) == 'x' && peek(3) == 'm' && peek(4) == 'l')
        {
            m_pos += 5; // 消费 "<?xml"
            // 读到 "?>"
            size_t j = m_text.find("?>", m_pos);
            if (j == std::string::npos) { setError("XML 声明未闭合"); return; }
            m_pos = j + 2;
        }

        while (!atEnd())
        {
            skipSpace();
            if (atEnd()) break;
            if (peek() == '<' && peek(1) == '?')
            {
                // 处理指令 (非 xml)
                JZDomNodeDataPtr pi = parsePI();
                if (!pi) return;
                pi->parent = m_doc.get();
                m_doc->children.push_back(pi);
            }
            else if (peek() == '<' && peek(1) == '!' && peek(2) == '-' && peek(3) == '-')
            {
                JZDomNodeDataPtr c = parseComment();
                if (!c) return;
                c->parent = m_doc.get();
                m_doc->children.push_back(c);
            }
            else if (peek() == '<' && peek(1) == '!' && peek(2) == 'D' && peek(3) == 'O' && peek(4) == 'C')
            {
                JZDomNodeDataPtr dt = parseDocType();
                if (!dt) return;
                dt->parent = m_doc.get();
                m_doc->children.push_back(dt);
            }
            else if (peek() == '<' && (peek(1) != '/' && peek(1) != '!'))
            {
                return; // 根元素开始
            }
            else if (peek() == '<' && peek(1) == '/')
            {
                setError("根元素之前出现结束标签");
                return;
            }
            else
            {
                setError("XML 声明之前存在非法内容");
                return;
            }
        }
    }

    JZDomNodeDataPtr parseDocType()
    {
        m_pos += 9; // "<!DOCTYPE"
        skipSpace();
        std::string name = readName();
        if (name.empty()) return nullptr;

        auto dt = std::make_shared<JZDomNodeData>();
        dt->type = JZDomNodeData::Type::DocumentType;
        dt->name = name;

        // 读取到 '>' (内部子集 [...] 内的 '>' 需跳过)
        std::string rest;
        int bracket = 0;
        while (!atEnd())
        {
            char c = peek();
            if (c == '[') { bracket++; m_pos++; continue; }
            if (c == ']') { bracket--; m_pos++; continue; }
            if (c == '>' && bracket == 0) { m_pos++; break; }
            rest += next();
        }

        // 从 rest 中提取 PUBLIC / SYSTEM 标识符
        size_t q1 = rest.find_first_of("\"'");
        if (q1 != std::string::npos)
        {
            char q = rest[q1];
            size_t q1e = rest.find(q, q1 + 1);
            if (q1e != std::string::npos)
            {
                std::string s1 = rest.substr(q1 + 1, q1e - q1 - 1);
                std::string kw = str_trim(rest.substr(0, q1));
                if (kw == "PUBLIC")
                {
                    dt->publicId = s1;
                    size_t q2 = rest.find_first_of("\"'", q1e + 1);
                    if (q2 != std::string::npos)
                    {
                        char q2c = rest[q2];
                        size_t q2e = rest.find(q2c, q2 + 1);
                        if (q2e != std::string::npos)
                            dt->systemId = rest.substr(q2 + 1, q2e - q2 - 1);
                    }
                }
                else
                {
                    dt->systemId = s1; // SYSTEM 或裸字面量
                }
            }
        }
        return dt;
    }

    JZDomNodeDataPtr parseComment()
    {
        m_pos += 4; // "<!--"
        size_t j = m_text.find("-->", m_pos);
        if (j == std::string::npos) { setError("注释未闭合"); return nullptr; }
        std::string data = m_text.substr(m_pos, j - m_pos);
        m_pos = j + 3;
        auto n = std::make_shared<JZDomNodeData>();
        n->type = JZDomNodeData::Type::Comment;
        n->value = data;
        return n;
    }

    JZDomNodeDataPtr parsePI()
    {
        m_pos += 2; // "<?"
        std::string target = readName();
        if (target.empty()) return nullptr;
        skipSpace();
        // data 直到 "?>"
        std::string data;
        while (!atEnd())
        {
            if (peek() == '?' && peek(1) == '>') break;
            data += next();
        }
        if (!tryMatch("?>")) { setError("处理指令未闭合"); return nullptr; }
        data = str_trim(data);
        auto n = std::make_shared<JZDomNodeData>();
        n->type = JZDomNodeData::Type::ProcessingInstruction;
        n->name = target;
        n->value = data;
        return n;
    }

    JZDomNodeDataPtr parseElement()
    {
        m_pos += 1; // "<"
        std::string name = readName();
        if (name.empty()) return nullptr;

        auto el = std::make_shared<JZDomNodeData>();
        el->type = JZDomNodeData::Type::Element;
        el->name = name;

        // 属性
        while (!atEnd())
        {
            skipSpace();
            char c = peek();
            if (c == '>') { m_pos++; break; }
            if (c == '/' && peek(1) == '>') { m_pos += 2; /* 自闭合 */ parseContent(el, true); return el; }
            if (!isNameStart(c)) { setError("元素 " + name + " 属性解析错误"); return nullptr; }

            std::string aname = readName();
            skipSpace();
            if (peek() != '=') { setError("属性 " + aname + " 缺少 '='"); return nullptr; }
            m_pos++; // "="
            skipSpace();
            char q = peek();
            if (q != '"' && q != '\'') { setError("属性 " + aname + " 值须用引号包围"); return nullptr; }
            m_pos++; // 开引号
            std::string aval;
            while (!atEnd() && peek() != q)
                aval += next();
            if (peek() != q) { setError("属性 " + aname + " 值未闭合"); return nullptr; }
            m_pos++; // 闭引号
            aval = decodeEntities(aval);

            auto attr = std::make_shared<JZDomNodeData>();
            attr->type = JZDomNodeData::Type::Attribute;
            attr->name = aname;
            attr->value = aval;
            attr->parent = el.get();
            el->attributes.push_back(attr);
        }

        parseContent(el, false);
        if (!m_error.empty())
            return nullptr;
        return el;
    }

    // 解析子节点直到遇到 </name> (close=true) 或 EOF; close=true 表示自闭合元素无内容
    void parseContent(const JZDomNodeDataPtr& el, bool selfClosed)
    {
        if (selfClosed) return;

        while (!atEnd())
        {
            skipSpace();
            if (atEnd()) { setError("元素 " + el->name + " 未闭合"); return; }

            if (peek() == '<' && peek(1) == '/')
            {
                m_pos += 2; // "</"
                std::string closeName = readName();
                if (closeName != el->name) { setError("结束标签 </" + closeName + "> 与开始标签 <" + el->name + "> 不匹配"); return; }
                skipSpace();
                if (peek() != '>') { setError("结束标签 " + closeName + " 格式错误"); return; }
                m_pos++; // ">"
                return;
            }
            else if (peek() == '<' && peek(1) == '!' && peek(2) == '-' && peek(3) == '-')
            {
                JZDomNodeDataPtr c = parseComment();
                if (!c) return;
                c->parent = el.get();
                el->children.push_back(c);
            }
            else if (peek() == '<' && peek(1) == '!' && peek(2) == '[')
            {
                // <![CDATA[
                m_pos += 9;
                size_t j = m_text.find("]]>", m_pos);
                if (j == std::string::npos) { setError("CDATA 未闭合"); return; }
                std::string data = m_text.substr(m_pos, j - m_pos);
                m_pos = j + 3;
                auto n = std::make_shared<JZDomNodeData>();
                n->type = JZDomNodeData::Type::CDATASection;
                n->value = data;
                n->parent = el.get();
                el->children.push_back(n);
            }
            else if (peek() == '<' && peek(1) == '?')
            {
                JZDomNodeDataPtr pi = parsePI();
                if (!pi) return;
                pi->parent = el.get();
                el->children.push_back(pi);
            }
            else if (peek() == '<' && peek(1) != '/')
            {
                JZDomNodeDataPtr child = parseElement();
                if (!child) return;
                child->parent = el.get();
                el->children.push_back(child);
            }
            else if (peek() == '<' && peek(1) == '/')
            {
                // 已在上方面处理
                return;
            }
            else
            {
                // 文本节点
                std::string text = readUntil("<");
                std::string decoded = decodeEntities(text);
                if (!decoded.empty())
                {
                    auto n = std::make_shared<JZDomNodeData>();
                    n->type = JZDomNodeData::Type::Text;
                    n->value = decoded;
                    n->parent = el.get();
                    el->children.push_back(n);
                }
            }
        }
    }

    static std::string str_trim(const std::string& s)
    {
        size_t a = 0, b = s.size();
        while (a < b && isSpaceChar(s[a])) a++;
        while (b > a && isSpaceChar(s[b - 1])) b--;
        return s.substr(a, b - a);
    }
};

} // namespace

// ============================================================================
// JZDomNodeList
// ============================================================================
JZDomNodeList::JZDomNodeList() {}
JZDomNodeList::JZDomNodeList(const std::vector<JZDomNodeDataPtr>& nodes) : m_nodes(nodes) {}

bool JZDomNodeList::isEmpty() const { return m_nodes.empty(); }
int JZDomNodeList::count() const { return (int)m_nodes.size(); }
int JZDomNodeList::length() const { return (int)m_nodes.size(); }

JZDomNode JZDomNodeList::item(int index) const
{
    if (index < 0 || index >= (int)m_nodes.size())
        return JZDomNode();
    return JZDomNode(m_nodes[index]);
}

JZDomNode JZDomNodeList::operator[](int index) const { return item(index); }

// ============================================================================
// JZDomNamedNodeMap
// ============================================================================
JZDomNamedNodeMap::JZDomNamedNodeMap() {}
JZDomNamedNodeMap::JZDomNamedNodeMap(const std::vector<JZDomNodeDataPtr>& attrs) : m_nodes(attrs) {}

int JZDomNamedNodeMap::count() const { return (int)m_nodes.size(); }
int JZDomNamedNodeMap::length() const { return (int)m_nodes.size(); }

JZDomNode JZDomNamedNodeMap::item(int index) const
{
    if (index < 0 || index >= (int)m_nodes.size())
        return JZDomNode();
    return JZDomNode(m_nodes[index]);
}

JZDomNode JZDomNamedNodeMap::namedItem(const std::string& name) const
{
    for (const auto& a : m_nodes)
        if (a->name == name)
            return JZDomNode(a);
    return JZDomNode();
}

bool JZDomNamedNodeMap::contains(const std::string& name) const
{
    for (const auto& a : m_nodes)
        if (a->name == name)
            return true;
    return false;
}

JZDomNode JZDomNamedNodeMap::setNamedItem(const JZDomNode& node)
{
    if (!node.isAttr() || !node.m_data)
        return JZDomNode();
    for (auto& a : m_nodes)
    {
        if (a->name == node.m_data->name)
        {
            JZDomNode old(a);
            a = node.m_data;
            a->parent = node.m_data->parent; // 调用方负责设置 parent
            return old;
        }
    }
    m_nodes.push_back(node.m_data);
    return JZDomNode();
}

JZDomNode JZDomNamedNodeMap::removeNamedItem(const std::string& name)
{
    for (auto it = m_nodes.begin(); it != m_nodes.end(); ++it)
    {
        if ((*it)->name == name)
        {
            JZDomNode old(*it);
            m_nodes.erase(it);
            return old;
        }
    }
    return JZDomNode();
}

// ============================================================================
// JZDomNode
// ============================================================================
JZDomNode::JZDomNode() {}
JZDomNode::JZDomNode(const JZDomNodeDataPtr& data) : m_data(data) {}
JZDomNode::~JZDomNode() {}

bool JZDomNode::isNull() const { return !m_data; }

int JZDomNode::nodeType() const
{
    if (!m_data) return 0;
    switch (m_data->type)
    {
    case JZDomNodeData::Type::Document: return JZDomDocumentNode;
    case JZDomNodeData::Type::Element: return JZDomElementNode;
    case JZDomNodeData::Type::Attribute: return JZDomAttributeNode;
    case JZDomNodeData::Type::Text: return JZDomTextNode;
    case JZDomNodeData::Type::CDATASection: return JZDomCDATASectionNode;
    case JZDomNodeData::Type::Comment: return JZDomCommentNode;
    case JZDomNodeData::Type::ProcessingInstruction: return JZDomProcessingInstructionNode;
    case JZDomNodeData::Type::DocumentType: return JZDomDocumentTypeNode;
    case JZDomNodeData::Type::EntityReference: return JZDomEntityReferenceNode;
    case JZDomNodeData::Type::Notation: return JZDomNotationNode;
    case JZDomNodeData::Type::DocumentFragment: return JZDomDocumentFragmentNode;
    }
    return 0;
}

std::string JZDomNode::nodeName() const
{
    if (!m_data) return std::string();
    switch (m_data->type)
    {
    case JZDomNodeData::Type::Document: return "#document";
    case JZDomNodeData::Type::DocumentFragment: return "#document-fragment";
    case JZDomNodeData::Type::Element: return m_data->name;
    case JZDomNodeData::Type::Attribute: return m_data->name;
    case JZDomNodeData::Type::Text: return "#text";
    case JZDomNodeData::Type::CDATASection: return "#cdata-section";
    case JZDomNodeData::Type::Comment: return "#comment";
    case JZDomNodeData::Type::ProcessingInstruction: return m_data->name;
    case JZDomNodeData::Type::DocumentType: return m_data->name;
    case JZDomNodeData::Type::EntityReference: return m_data->name;
    case JZDomNodeData::Type::Notation: return m_data->name;
    }
    return std::string();
}

std::string JZDomNode::nodeValue() const
{
    if (!m_data) return std::string();
    switch (m_data->type)
    {
    case JZDomNodeData::Type::Attribute:
    case JZDomNodeData::Type::Text:
    case JZDomNodeData::Type::CDATASection:
    case JZDomNodeData::Type::Comment:
    case JZDomNodeData::Type::ProcessingInstruction:
        return m_data->value;
    default:
        return std::string();
    }
}

void JZDomNode::setNodeValue(const std::string& v)
{
    if (!m_data) return;
    switch (m_data->type)
    {
    case JZDomNodeData::Type::Attribute:
    case JZDomNodeData::Type::Text:
    case JZDomNodeData::Type::CDATASection:
    case JZDomNodeData::Type::Comment:
    case JZDomNodeData::Type::ProcessingInstruction:
        m_data->value = v;
        break;
    default:
        break;
    }
}

JZDomNode JZDomNode::parentNode() const
{
    if (!m_data || !m_data->parent) return JZDomNode();
    return JZDomNode(m_data->parent->shared_from_this());
}

JZDomNode JZDomNode::firstChild() const
{
    if (!m_data || m_data->children.empty()) return JZDomNode();
    return JZDomNode(m_data->children.front());
}

JZDomNode JZDomNode::lastChild() const
{
    if (!m_data || m_data->children.empty()) return JZDomNode();
    return JZDomNode(m_data->children.back());
}

JZDomNode JZDomNode::nextSibling() const
{
    if (!m_data || !m_data->parent) return JZDomNode();
    const auto& sibs = m_data->parent->children;
    for (size_t i = 0; i + 1 < sibs.size(); i++)
    {
        if (sibs[i].get() == m_data.get())
            return JZDomNode(sibs[i + 1]);
    }
    return JZDomNode();
}

JZDomNode JZDomNode::previousSibling() const
{
    if (!m_data || !m_data->parent) return JZDomNode();
    const auto& sibs = m_data->parent->children;
    for (size_t i = 1; i < sibs.size(); i++)
    {
        if (sibs[i].get() == m_data.get())
            return JZDomNode(sibs[i - 1]);
    }
    return JZDomNode();
}

JZDomNodeList JZDomNode::childNodes() const
{
    if (!m_data) return JZDomNodeList();
    return JZDomNodeList(m_data->children);
}

JZDomNamedNodeMap JZDomNode::attributes() const
{
    if (!m_data) return JZDomNamedNodeMap();
    return JZDomNamedNodeMap(m_data->attributes);
}

bool JZDomNode::hasChildNodes() const { return m_data && !m_data->children.empty(); }
bool JZDomNode::hasAttributes() const { return m_data && !m_data->attributes.empty(); }

bool JZDomNode::isElement() const { return m_data && m_data->type == JZDomNodeData::Type::Element; }
bool JZDomNode::isAttr() const { return m_data && m_data->type == JZDomNodeData::Type::Attribute; }
bool JZDomNode::isText() const { return m_data && m_data->type == JZDomNodeData::Type::Text; }
bool JZDomNode::isCDATASection() const { return m_data && m_data->type == JZDomNodeData::Type::CDATASection; }
bool JZDomNode::isComment() const { return m_data && m_data->type == JZDomNodeData::Type::Comment; }
bool JZDomNode::isDocument() const { return m_data && m_data->type == JZDomNodeData::Type::Document; }
bool JZDomNode::isDocumentFragment() const { return m_data && m_data->type == JZDomNodeData::Type::DocumentFragment; }
bool JZDomNode::isProcessingInstruction() const { return m_data && m_data->type == JZDomNodeData::Type::ProcessingInstruction; }
bool JZDomNode::isDocumentType() const { return m_data && m_data->type == JZDomNodeData::Type::DocumentType; }
bool JZDomNode::isEntityReference() const { return m_data && m_data->type == JZDomNodeData::Type::EntityReference; }
bool JZDomNode::isNotation() const { return m_data && m_data->type == JZDomNodeData::Type::Notation; }
bool JZDomNode::isCharacterData() const
{
    return isText() || isCDATASection() || isComment();
}

JZDomElement JZDomNode::toElement() const { return JZDomElement(m_data); }
JZDomAttr JZDomNode::toAttr() const { return JZDomAttr(m_data); }
JZDomText JZDomNode::toText() const { return JZDomText(m_data); }
JZDomCDATASection JZDomNode::toCDATASection() const { return JZDomCDATASection(m_data); }
JZDomComment JZDomNode::toComment() const { return JZDomComment(m_data); }
JZDomDocument JZDomNode::toDocument() const { return JZDomDocument(m_data); }
JZDomProcessingInstruction JZDomNode::toProcessingInstruction() const { return JZDomProcessingInstruction(m_data); }
JZDomDocumentType JZDomNode::toDocumentType() const { return JZDomDocumentType(m_data); }
JZDomEntityReference JZDomNode::toEntityReference() const { return JZDomEntityReference(m_data); }
JZDomNotation JZDomNode::toNotation() const { return JZDomNotation(m_data); }
JZDomDocumentFragment JZDomNode::toDocumentFragment() const { return JZDomDocumentFragment(m_data); }

JZDomNode JZDomNode::namedItem(const std::string& name) const
{
    if (!m_data) return JZDomNode();
    // 对齐 Qt QDomNode::namedItem: 返回首个指定 tagName 的子元素。
    for (const auto& c : m_data->children) {
        if (c->type == JZDomNodeData::Type::Element && c->name == name)
            return JZDomNode(c);
    }
    return JZDomNode();
}

void collectElementsByTagName(const JZDomNodeDataPtr& node, const std::string& tag, std::vector<JZDomNodeDataPtr>& out)
{
    for (const auto& c : node->children)
    {
        if (c->type == JZDomNodeData::Type::Element)
        {
            if (c->name == tag)
                out.push_back(c);
            collectElementsByTagName(c, tag, out);
        }
    }
}

JZDomNodeList JZDomNode::elementsByTagName(const std::string& tagname) const
{
    if (!m_data) return JZDomNodeList();
    std::vector<JZDomNodeDataPtr> out;
    collectElementsByTagName(m_data, tagname, out);
    return JZDomNodeList(out);
}

JZDomNode JZDomNode::insertBefore(const JZDomNode& newChild, const JZDomNode& refChild)
{
    if (!m_data || !newChild.m_data || newChild.m_data->type == JZDomNodeData::Type::Document)
        return JZDomNode();
    if (newChild.m_data->parent)
    {
        removeFromVector(newChild.m_data->parent->children, newChild.m_data);
        removeFromVector(newChild.m_data->parent->attributes, newChild.m_data);
        newChild.m_data->parent = nullptr;
    }
    if (!refChild.m_data)
    {
        newChild.m_data->parent = m_data.get();
        m_data->children.push_back(newChild.m_data);
        return newChild;
    }
    auto& sibs = m_data->children;
    auto it = std::find(sibs.begin(), sibs.end(), refChild.m_data);
    newChild.m_data->parent = m_data.get();
    if (it == sibs.end())
        sibs.push_back(newChild.m_data);
    else
        sibs.insert(it, newChild.m_data);
    return newChild;
}

JZDomNode JZDomNode::replaceChild(const JZDomNode& newChild, const JZDomNode& oldChild)
{
    if (!m_data || !newChild.m_data || !oldChild.m_data)
        return JZDomNode();
    auto& sibs = m_data->children;
    auto it = std::find(sibs.begin(), sibs.end(), oldChild.m_data);
    if (it == sibs.end())
        return JZDomNode();
    if (newChild.m_data->parent)
    {
        removeFromVector(newChild.m_data->parent->children, newChild.m_data);
        removeFromVector(newChild.m_data->parent->attributes, newChild.m_data);
    }
    newChild.m_data->parent = m_data.get();
    *it = newChild.m_data;
    oldChild.m_data->parent = nullptr;
    return oldChild;
}

JZDomNode JZDomNode::removeChild(const JZDomNode& oldChild)
{
    if (!m_data || !oldChild.m_data)
        return JZDomNode();
    auto& sibs = m_data->children;
    auto it = std::find(sibs.begin(), sibs.end(), oldChild.m_data);
    if (it == sibs.end())
        return JZDomNode();
    sibs.erase(it);
    oldChild.m_data->parent = nullptr;
    return oldChild;
}

JZDomNode JZDomNode::appendChild(const JZDomNode& newChild)
{
    if (!m_data || !newChild.m_data || newChild.m_data->type == JZDomNodeData::Type::Document)
        return JZDomNode();
    if (newChild.m_data->parent)
    {
        removeFromVector(newChild.m_data->parent->children, newChild.m_data);
        removeFromVector(newChild.m_data->parent->attributes, newChild.m_data);
        newChild.m_data->parent = nullptr;
    }
    newChild.m_data->parent = m_data.get();
    m_data->children.push_back(newChild.m_data);
    return newChild;
}

JZDomNodeDataPtr cloneData(const JZDomNodeDataPtr& src, bool deep)
{
    auto n = std::make_shared<JZDomNodeData>();
    n->type = src->type;
    n->name = src->name;
    n->value = src->value;
    n->publicId = src->publicId;
    n->systemId = src->systemId;
    n->parent = nullptr;
    if (deep)
    {
        for (const auto& c : src->children)
        {
            auto cc = cloneData(c, true);
            cc->parent = n.get();
            n->children.push_back(cc);
        }
    }
    for (const auto& a : src->attributes)
    {
        auto ca = cloneData(a, false);
        ca->parent = n.get();
        n->attributes.push_back(ca);
    }
    return n;
}

JZDomNode JZDomNode::cloneNode(bool deep) const
{
    if (!m_data) return JZDomNode();
    return JZDomNode(cloneData(m_data, deep));
}

JZDomDocument JZDomNode::ownerDocument() const
{
    if (!m_data) return JZDomDocument();
    JZDomNodeData* p = m_data.get();
    while (p && p->type != JZDomNodeData::Type::Document)
        p = p->parent;
    if (!p) return JZDomDocument();
    return JZDomDocument(p->shared_from_this());
}

std::string JZDomNode::toString(int indent) const
{
    return writeNodeToString(m_data, indent);
}

// ============================================================================
// JZDomCharacterData
// ============================================================================
JZDomCharacterData::JZDomCharacterData() {}
JZDomCharacterData::JZDomCharacterData(const JZDomNodeDataPtr& data) : JZDomNode(data) {}

std::string JZDomCharacterData::data() const { return m_data ? m_data->value : std::string(); }
void JZDomCharacterData::setData(const std::string& d) { if (m_data) m_data->value = d; }
int JZDomCharacterData::length() const { return m_data ? (int)m_data->value.size() : 0; }

std::string JZDomCharacterData::substringData(int offset, int count) const
{
    if (!m_data) return std::string();
    if (offset < 0 || offset >= (int)m_data->value.size()) return std::string();
    if (count < 0) count = (int)m_data->value.size();
    return m_data->value.substr(offset, count);
}

void JZDomCharacterData::appendData(const std::string& arg) { if (m_data) m_data->value += arg; }
void JZDomCharacterData::insertData(int offset, const std::string& arg)
{
    if (!m_data) return;
    if (offset < 0 || offset > (int)m_data->value.size()) offset = (int)m_data->value.size();
    m_data->value.insert(offset, arg);
}
void JZDomCharacterData::deleteData(int offset, int count)
{
    if (!m_data) return;
    if (offset < 0 || offset >= (int)m_data->value.size()) return;
    if (count < 0 || offset + count > (int)m_data->value.size()) count = (int)m_data->value.size() - offset;
    m_data->value.erase(offset, count);
}
void JZDomCharacterData::replaceData(int offset, int count, const std::string& arg)
{
    if (!m_data) return;
    if (offset < 0 || offset >= (int)m_data->value.size()) return;
    if (count < 0 || offset + count > (int)m_data->value.size()) count = (int)m_data->value.size() - offset;
    m_data->value.replace(offset, count, arg);
}

// ============================================================================
// JZDomElement
// ============================================================================
JZDomElement::JZDomElement() {}
JZDomElement::JZDomElement(const JZDomNodeDataPtr& data) : JZDomNode(data) {}

std::string JZDomElement::tagName() const { return m_data ? m_data->name : std::string(); }
void JZDomElement::setTagName(const std::string& name) { if (m_data) m_data->name = name; }

std::string JZDomElement::attribute(const std::string& name, const std::string& defValue) const
{
    if (!m_data) return defValue;
    for (const auto& a : m_data->attributes)
        if (a->name == name) return a->value;
    return defValue;
}

void JZDomElement::setAttribute(const std::string& name, const std::string& value)
{
    if (!m_data) return;
    for (auto& a : m_data->attributes)
    {
        if (a->name == name) { a->value = value; return; }
    }
    auto attr = std::make_shared<JZDomNodeData>();
    attr->type = JZDomNodeData::Type::Attribute;
    attr->name = name;
    attr->value = value;
    attr->parent = m_data.get();
    m_data->attributes.push_back(attr);
}

void JZDomElement::setAttribute(const std::string& name, int value)
{
    setAttribute(name, std::to_string(value));
}

void JZDomElement::setAttribute(const std::string& name, double value)
{
    std::ostringstream oss;
    oss << value;
    setAttribute(name, oss.str());
}

void JZDomElement::removeAttribute(const std::string& name)
{
    if (!m_data) return;
    for (auto it = m_data->attributes.begin(); it != m_data->attributes.end(); ++it)
    {
        if ((*it)->name == name) { m_data->attributes.erase(it); return; }
    }
}

bool JZDomElement::hasAttribute(const std::string& name) const
{
    if (!m_data) return false;
    for (const auto& a : m_data->attributes)
        if (a->name == name) return true;
    return false;
}

JZDomAttr JZDomElement::attributeNode(const std::string& name) const
{
    if (!m_data) return JZDomAttr();
    for (const auto& a : m_data->attributes)
        if (a->name == name) return JZDomAttr(a);
    return JZDomAttr();
}

JZDomAttr JZDomElement::setAttributeNode(const JZDomAttr& newAttr)
{
    if (!m_data || !newAttr.m_data || newAttr.m_data->type != JZDomNodeData::Type::Attribute)
        return JZDomAttr();
    for (auto& a : m_data->attributes)
    {
        if (a->name == newAttr.m_data->name)
        {
            JZDomAttr old(a);
            a = newAttr.m_data;
            a->parent = m_data.get();
            return old;
        }
    }
    newAttr.m_data->parent = m_data.get();
    m_data->attributes.push_back(newAttr.m_data);
    return JZDomAttr();
}

JZDomAttr JZDomElement::removeAttributeNode(const JZDomAttr& oldAttr)
{
    if (!m_data || !oldAttr.m_data) return JZDomAttr();
    for (auto it = m_data->attributes.begin(); it != m_data->attributes.end(); ++it)
    {
        if (*it == oldAttr.m_data)
        {
            JZDomAttr old(*it);
            m_data->attributes.erase(it);
            old.m_data->parent = nullptr;
            return old;
        }
    }
    return JZDomAttr();
}

std::string JZDomElement::text() const
{
    std::string out;
    if (!m_data) return out;
    for (const auto& c : m_data->children)
    {
        if (c->type == JZDomNodeData::Type::Text || c->type == JZDomNodeData::Type::CDATASection)
            out += c->value;
        else if (c->type == JZDomNodeData::Type::Element)
            out += JZDomElement(c).text();
    }
    return out;
}

void JZDomElement::setText(const std::string& text)
{
    if (!m_data) return;
    m_data->children.clear();
    auto t = std::make_shared<JZDomNodeData>();
    t->type = JZDomNodeData::Type::Text;
    t->value = text;
    t->parent = m_data.get();
    m_data->children.push_back(t);
}

JZDomNodeList JZDomElement::elementsByTagName(const std::string& tagname) const
{
    return JZDomNode::elementsByTagName(tagname);
}

// ============================================================================
// JZDomAttr
// ============================================================================
JZDomAttr::JZDomAttr() {}
JZDomAttr::JZDomAttr(const JZDomNodeDataPtr& data) : JZDomNode(data) {}

std::string JZDomAttr::name() const { return m_data ? m_data->name : std::string(); }
std::string JZDomAttr::value() const { return m_data ? m_data->value : std::string(); }
void JZDomAttr::setValue(const std::string& v) { if (m_data) m_data->value = v; }
bool JZDomAttr::specified() const { return true; }

JZDomElement JZDomAttr::ownerElement() const
{
    if (!m_data || !m_data->parent) return JZDomElement();
    if (m_data->parent->type != JZDomNodeData::Type::Element)
        return JZDomElement();
    return JZDomElement(m_data->parent->shared_from_this());
}

// ============================================================================
// JZDomText
// ============================================================================
JZDomText::JZDomText() {}
JZDomText::JZDomText(const JZDomNodeDataPtr& data) : JZDomCharacterData(data) {}

JZDomText JZDomText::splitText(int offset)
{
    JZDomText empty;
    if (!m_data) return empty;
    if (offset < 0) offset = 0;
    if (offset > (int)m_data->value.size()) offset = (int)m_data->value.size();

    std::string second = m_data->value.substr(offset);
    m_data->value = m_data->value.substr(0, offset);

    auto n = std::make_shared<JZDomNodeData>();
    n->type = JZDomNodeData::Type::Text;
    n->value = second;
    if (m_data->parent)
    {
        n->parent = m_data->parent;
        auto& sibs = m_data->parent->children;
        auto it = std::find(sibs.begin(), sibs.end(), m_data);
        sibs.insert(it + 1, n);
    }
    return JZDomText(n);
}

// ============================================================================
// JZDomCDATASection / JZDomComment
// ============================================================================
JZDomCDATASection::JZDomCDATASection() {}
JZDomCDATASection::JZDomCDATASection(const JZDomNodeDataPtr& data) : JZDomCharacterData(data) {}

JZDomComment::JZDomComment() {}
JZDomComment::JZDomComment(const JZDomNodeDataPtr& data) : JZDomCharacterData(data) {}

// ============================================================================
// JZDomProcessingInstruction
// ============================================================================
JZDomProcessingInstruction::JZDomProcessingInstruction() {}
JZDomProcessingInstruction::JZDomProcessingInstruction(const JZDomNodeDataPtr& data) : JZDomNode(data) {}

std::string JZDomProcessingInstruction::target() const { return m_data ? m_data->name : std::string(); }
std::string JZDomProcessingInstruction::data() const { return m_data ? m_data->value : std::string(); }
void JZDomProcessingInstruction::setData(const std::string& d) { if (m_data) m_data->value = d; }

// ============================================================================
// JZDomDocumentType / JZDomEntityReference / JZDomNotation / JZDomDocumentFragment
// ============================================================================
JZDomDocumentType::JZDomDocumentType() {}
JZDomDocumentType::JZDomDocumentType(const JZDomNodeDataPtr& data) : JZDomNode(data) {}

std::string JZDomDocumentType::name() const { return m_data ? m_data->name : std::string(); }
std::string JZDomDocumentType::publicId() const { return m_data ? m_data->publicId : std::string(); }
std::string JZDomDocumentType::systemId() const { return m_data ? m_data->systemId : std::string(); }

JZDomEntityReference::JZDomEntityReference() {}
JZDomEntityReference::JZDomEntityReference(const JZDomNodeDataPtr& data) : JZDomNode(data) {}
std::string JZDomEntityReference::name() const { return m_data ? m_data->name : std::string(); }

JZDomNotation::JZDomNotation() {}
JZDomNotation::JZDomNotation(const JZDomNodeDataPtr& data) : JZDomNode(data) {}
std::string JZDomNotation::name() const { return m_data ? m_data->name : std::string(); }
std::string JZDomNotation::publicId() const { return m_data ? m_data->publicId : std::string(); }
std::string JZDomNotation::systemId() const { return m_data ? m_data->systemId : std::string(); }

JZDomDocumentFragment::JZDomDocumentFragment() {}
JZDomDocumentFragment::JZDomDocumentFragment(const JZDomNodeDataPtr& data) : JZDomNode(data) {}

// ============================================================================
// JZDomDocument
// ============================================================================
JZDomDocument::JZDomDocument()
{
    m_data = std::make_shared<JZDomNodeData>();
    m_data->type = JZDomNodeData::Type::Document;
}

JZDomDocument::JZDomDocument(const JZDomNodeDataPtr& data) : JZDomNode(data) {}

void JZDomDocument::clear()
{
    m_data = std::make_shared<JZDomNodeData>();
    m_data->type = JZDomNodeData::Type::Document;
}

bool JZDomDocument::setContent(const std::string& text, std::string* errorMsg, int* errorLine, int* errorColumn)
{
    JZXmlParser parser(text);
    JZDomNodeDataPtr doc;
    if (!parser.parse(doc, errorMsg, errorLine, errorColumn))
        return false;
    m_data = doc;
    return true;
}

bool JZDomDocument::setContent(const char* text, std::string* errorMsg, int* errorLine, int* errorColumn)
{
    return setContent(std::string(text ? text : ""), errorMsg, errorLine, errorColumn);
}

bool JZDomDocument::isEmpty() const
{
    if (!m_data) return true;
    for (const auto& c : m_data->children)
        if (c->type == JZDomNodeData::Type::Element)
            return false;
    return true;
}

JZDomElement JZDomDocument::documentElement() const
{
    if (!m_data) return JZDomElement();
    for (const auto& c : m_data->children)
        if (c->type == JZDomNodeData::Type::Element)
            return JZDomElement(c);
    return JZDomElement();
}

JZDomDocumentType JZDomDocument::doctype() const
{
    if (!m_data) return JZDomDocumentType();
    for (const auto& c : m_data->children)
        if (c->type == JZDomNodeData::Type::DocumentType)
            return JZDomDocumentType(c);
    return JZDomDocumentType();
}

JZDomElement JZDomDocument::createElement(const std::string& tagName)
{
    auto n = std::make_shared<JZDomNodeData>();
    n->type = JZDomNodeData::Type::Element;
    n->name = tagName;
    return JZDomElement(n);
}

JZDomText JZDomDocument::createTextNode(const std::string& data)
{
    auto n = std::make_shared<JZDomNodeData>();
    n->type = JZDomNodeData::Type::Text;
    n->value = data;
    return JZDomText(n);
}

JZDomComment JZDomDocument::createComment(const std::string& data)
{
    auto n = std::make_shared<JZDomNodeData>();
    n->type = JZDomNodeData::Type::Comment;
    n->value = data;
    return JZDomComment(n);
}

JZDomCDATASection JZDomDocument::createCDATASection(const std::string& data)
{
    auto n = std::make_shared<JZDomNodeData>();
    n->type = JZDomNodeData::Type::CDATASection;
    n->value = data;
    return JZDomCDATASection(n);
}

JZDomAttr JZDomDocument::createAttribute(const std::string& name)
{
    auto n = std::make_shared<JZDomNodeData>();
    n->type = JZDomNodeData::Type::Attribute;
    n->name = name;
    return JZDomAttr(n);
}

JZDomProcessingInstruction JZDomDocument::createProcessingInstruction(const std::string& target, const std::string& data)
{
    auto n = std::make_shared<JZDomNodeData>();
    n->type = JZDomNodeData::Type::ProcessingInstruction;
    n->name = target;
    n->value = data;
    return JZDomProcessingInstruction(n);
}

JZDomDocumentFragment JZDomDocument::createDocumentFragment()
{
    auto n = std::make_shared<JZDomNodeData>();
    n->type = JZDomNodeData::Type::DocumentFragment;
    return JZDomDocumentFragment(n);
}

JZDomEntityReference JZDomDocument::createEntityReference(const std::string& name)
{
    auto n = std::make_shared<JZDomNodeData>();
    n->type = JZDomNodeData::Type::EntityReference;
    n->name = name;
    return JZDomEntityReference(n);
}

JZDomNode JZDomDocument::importNode(const JZDomNode& importedNode, bool deep)
{
    if (!importedNode.m_data) return JZDomNode();
    return JZDomNode(cloneData(importedNode.m_data, deep));
}

JZDomNodeList JZDomDocument::elementsByTagName(const std::string& tagname) const
{
    return JZDomNode::elementsByTagName(tagname);
}

std::string JZDomDocument::toString(int indent) const
{
    return writeNodeToString(m_data, indent);
}
