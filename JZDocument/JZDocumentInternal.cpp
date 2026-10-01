#include "JZDocumentInternal.h"

#include <cmath>
#include <cctype>

#include "JZCore/JZStd.h"

namespace JZDocInternal
{

bool readXml(JZUnZip &zip, const std::string &entry, JZDomDocument &doc, std::string &err)
{
    if (!zip.contains(entry))
    {
        err = "missing zip entry: " + entry;
        return false;
    }

    JZResult<JZByteArray> data = zip.extractToMemory(entry);
    if (data.isError())
    {
        err = "extract " + entry + " failed: " + data.errorMessage();
        return false;
    }

    const JZByteArray &buf = data.value();
    std::string xml(buf.constData(), (size_t)buf.size());
    std::string parseErr;
    if (!doc.setContent(xml, &parseErr))
    {
        err = "parse " + entry + " failed: " + parseErr;
        return false;
    }
    return true;
}

std::string localName(const std::string &tag)
{
    size_t pos = tag.find(':');
    if (pos == std::string::npos)
        return tag;
    return tag.substr(pos + 1);
}

std::string attrValue(const JZDomElement &e, const std::string &name)
{
    if (e.isNull())
        return std::string();

    std::string value = e.attribute(name);
    if (!value.empty())
        return value;

    // 退化匹配: 本地名相同(忽略前缀 / 大小写)即认为命中
    std::string want = str_toLower(localName(name));
    JZDomNamedNodeMap attrs = e.attributes();
    for (int i = 0; i < attrs.count(); i++)
    {
        JZDomAttr attr = attrs.item(i).toAttr();
        if (str_toLower(localName(attr.name())) == want)
            return attr.value();
    }
    return std::string();
}

std::vector<JZDomElement> childElements(const JZDomNode &node, const std::string &tag)
{
    std::vector<JZDomElement> list;
    if (node.isNull())
        return list;

    std::string want = localName(tag);
    JZDomNodeList children = node.childNodes();
    for (int i = 0; i < children.count(); i++)
    {
        JZDomNode child = children.item(i);
        if (!child.isElement())
            continue;
        JZDomElement e = child.toElement();
        if (tag.empty() || localName(e.tagName()) == want)
            list.push_back(e);
    }
    return list;
}

JZDomElement childElement(const JZDomNode &node, const std::string &tag)
{
    std::vector<JZDomElement> list = childElements(node, tag);
    if (list.empty())
        return JZDomElement();
    return list[0];
}

void collectTextNodes(const JZDomNode &node, std::string &out)
{
    if (node.isNull())
        return;

    JZDomNodeList children = node.childNodes();
    for (int i = 0; i < children.count(); i++)
    {
        JZDomNode child = children.item(i);
        if (child.isText())
        {
            out += child.nodeValue();
            continue;
        }
        if (!child.isElement())
            continue;

        JZDomElement e = child.toElement();
        std::string tag = localName(e.tagName());
        if (tag == "rPh" || tag == "phoneticPr")  // 日文注音, 不是正文
            continue;
        if (tag == "t")
        {
            out += e.text();
            continue;
        }
        collectTextNodes(e, out);
    }
}

void collectParagraphText(const JZDomNode &node, std::string &out)
{
    if (node.isNull())
        return;

    JZDomNodeList children = node.childNodes();
    for (int i = 0; i < children.count(); i++)
    {
        JZDomNode child = children.item(i);
        if (child.isText())
        {
            out += child.nodeValue();
            continue;
        }
        if (!child.isElement())
            continue;

        JZDomElement e = child.toElement();
        std::string tag = localName(e.tagName());
        if (tag == "t")
        {
            out += e.text();
            continue;
        }
        if (tag == "tab")
        {
            out += "\t";
            continue;
        }
        if (tag == "br" || tag == "cr")
        {
            out += "\n";
            continue;
        }
        if (tag == "instrText" || tag == "delText")  // 域指令 / 修订删除的文本
            continue;

        collectParagraphText(e, out);
    }
}

JZDomElement findElement(const JZDomNode &node, const std::string &tag)
{
    std::vector<JZDomElement> list;
    findElements(node, tag, list);
    if (list.empty())
        return JZDomElement();
    return list[0];
}

void findElements(const JZDomNode &node, const std::string &tag, std::vector<JZDomElement> &out)
{
    if (node.isNull())
        return;

    std::string want = localName(tag);
    JZDomNodeList children = node.childNodes();
    for (int i = 0; i < children.count(); i++)
    {
        JZDomNode child = children.item(i);
        if (!child.isElement())
            continue;

        JZDomElement e = child.toElement();
        if (localName(e.tagName()) == want)
            out.push_back(e);
        findElements(e, tag, out);
    }
}

int columnFromRef(const std::string &ref)
{
    int col = 0;
    bool hasLetter = false;
    for (size_t i = 0; i < ref.size(); i++)
    {
        char c = ref[i];
        if (c >= 'A' && c <= 'Z')
        {
            col = col * 26 + (c - 'A' + 1);
            hasLetter = true;
        }
        else if (c >= 'a' && c <= 'z')
        {
            col = col * 26 + (c - 'a' + 1);
            hasLetter = true;
        }
        else
        {
            break;
        }
    }
    if (!hasLetter)
        return -1;
    return col - 1;
}

std::string resolveTarget(const std::string &baseDir, const std::string &target)
{
    std::string t = target;
    str_replace(t, "\\", "/");

    std::string path;
    if (!t.empty() && t[0] == '/')
        path = t.substr(1);
    else
        path = baseDir + t;

    std::vector<std::string> parts = str_split(path, "/", JZ::SkipEmptyParts);
    std::vector<std::string> out;
    for (size_t i = 0; i < parts.size(); i++)
    {
        if (parts[i] == ".")
            continue;
        if (parts[i] == "..")
        {
            if (!out.empty())
                out.pop_back();
            continue;
        }
        out.push_back(parts[i]);
    }
    return str_join(out, "/");
}

std::string formatNumber(double v)
{
    if (v == std::floor(v) && std::fabs(v) < 1e15)
        return str_asprintf("%lld", (long long)v);
    return str_asprintf("%.10g", v);
}

// 天数(自 1970-01-01) → 年月日。Howard Hinnant 的 civil_from_days 算法。
static void civilFromDays(int z, int &y, unsigned &m, unsigned &d)
{
    z += 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (int)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
}

std::string serialToDate(double serial)
{
    if (serial < 0)
        return std::string();

    // 纯时间(serial < 1, 如 0.5 = 12:00:00)
    if (serial < 1.0)
    {
        int total = (int)std::floor(serial * 86400.0 + 0.5);
        if (total >= 86400)
            total = 86399;
        return str_asprintf("%02d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
    }

    int days = (int)std::floor(serial);
    double frac = serial - days;
    // 1970-01-01 = Excel serial 25569
    int y;
    unsigned m, d;
    civilFromDays(days - 25569, y, m, d);

    std::string out = str_asprintf("%04d-%02u-%02u", y, m, d);
    int total = (int)std::floor(frac * 86400.0 + 0.5);
    if (total > 0)
    {
        if (total >= 86400)
            total = 86399;
        out += str_asprintf(" %02d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
    }
    return out;
}

// 去掉转义(\x) / 字面量("...") / 条件段([...]) / 对齐(_x, *x) 后, 是否含日期时间占位符
static bool formatCodeHasDateTime(const std::string &formatCode)
{
    std::string clean;
    for (size_t i = 0; i < formatCode.size(); i++)
    {
        char c = formatCode[i];
        if (c == '\\' || c == '_' || c == '*')
        {
            i++;
            continue;
        }
        if (c == '"')
        {
            i++;
            while (i < formatCode.size() && formatCode[i] != '"')
                i++;
            continue;
        }
        if (c == '[')
        {
            while (i < formatCode.size() && formatCode[i] != ']')
                i++;
            continue;
        }
        clean += (char)std::tolower((unsigned char)c);
    }
    // 'm' 既可能是月也可能是分, 歧义太大, 只看 y / d / h / s
    return clean.find_first_of("ydhs") != std::string::npos;
}

bool isDateNumFmt(int numFmtId, const std::string &formatCode)
{
    // builtin: 14-22 / 27-36 / 45-47 / 50-58 为日期时间格式
    if ((numFmtId >= 14 && numFmtId <= 22) ||
        (numFmtId >= 27 && numFmtId <= 36) ||
        (numFmtId >= 45 && numFmtId <= 47) ||
        (numFmtId >= 50 && numFmtId <= 58))
    {
        return true;
    }
    return formatCodeHasDateTime(formatCode);
}

std::string markdownCell(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++)
    {
        char c = text[i];
        if (c == '|')
            out += "\\|";
        else if (c == '\r')
            continue;
        else if (c == '\n')
            out += "<br>";
        else if (c == '\t')
            out += " ";
        else
            out += c;
    }
    return out;
}

std::string trimmed(const std::string &text)
{
    return str_trimmed(text);
}

std::string trimmedRight(const std::string &text)
{
    size_t end = text.size();
    while (end > 0)
    {
        char c = text[end - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            end--;
        else
            break;
    }
    return text.substr(0, end);
}

std::string fileExt(const std::string &name)
{
    size_t pos = name.find_last_of('.');
    if (pos == std::string::npos)
        return std::string();
    std::string ext = name.substr(pos + 1);
    // 路径分隔符之后才出现的 "." 不算后缀
    if (ext.find_first_of("/\\") != std::string::npos)
        return std::string();
    return str_toLower(ext);
}

}  // namespace JZDocInternal
