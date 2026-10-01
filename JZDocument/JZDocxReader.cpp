#include "JZDocument.h"
#include "JZDocumentInternal.h"

#include "JZCore/JZStd.h"

// ============================================================================
// docx 提取
//
// docx = zip + OOXML。关注这几样东西:
//   word/document.xml               正文: <w:p> 段落 / <w:tbl> 表格
//   word/styles.xml                 样式表: styleId → 名字 + 大纲级别(判标题层级)
//   word/_rels/document.xml.rels    关系表: r:id → 资源路径(图片 rId → word/media/*)
//   word/media/*                    内嵌图片原始字节
//
// 只做「提取」: 不渲染、不写回。段落内遇到 <w:drawing>/<w:pict> 引用的图片,
// 按出现顺序插一个 Image 块(同一个 rId 多处引用只抽一次, 复用同一个 images 下标)。
// ============================================================================

namespace
{
struct DocxStyle
{
    std::string name;
    int outlineLvl = -1;   // -1 = 非标题; 0 → 一级标题
};

// "heading 2" / "标题 2" / "Heading2" → 2; 不是标题返回 0
int headingLevelFromName(const std::string &name)
{
    std::string lower = str_toLower(str_trimmed(name));
    if (!str_contains(lower, "heading") && !str_contains(lower, "标题"))
        return 0;

    // 取尾部连续数字
    size_t end = lower.size();
    while (end > 0 && lower[end - 1] >= '0' && lower[end - 1] <= '9')
        end--;
    if (end == lower.size())
        return 1;   // 有 heading 但没数字, 当成一级

    std::string digits = lower.substr(end);
    bool ok = false;
    int level = str_toInt(digits, &ok);
    if (!ok || level < 1 || level > 9)
        return 1;
    return level;
}

void parseStyles(JZUnZip &zip, std::map<std::string, DocxStyle> &styles)
{
    JZDomDocument doc;
    std::string err;
    if (!JZDocInternal::readXml(zip, "word/styles.xml", doc, err))
        return;   // 样式表缺失不致命: 退化为无标题层级

    std::vector<JZDomElement> list = JZDocInternal::childElements(doc.documentElement(), "w:style");
    for (size_t i = 0; i < list.size(); i++)
    {
        JZDomElement style = list[i];
        std::string id = JZDocInternal::attrValue(style, "styleId");
        if (id.empty())
            continue;

        DocxStyle info;
        info.name = JZDocInternal::attrValue(JZDocInternal::childElement(style, "w:name"), "val");

        JZDomElement pPr = JZDocInternal::childElement(style, "w:pPr");
        JZDomElement outline = JZDocInternal::childElement(pPr, "w:outlineLvl");
        if (!outline.isNull())
        {
            bool ok = false;
            int lvl = str_toInt(JZDocInternal::attrValue(outline, "val"), &ok);
            if (ok)
                info.outlineLvl = lvl;
        }
        styles[id] = info;
    }
}

// r:id → 包内路径
void parseRels(JZUnZip &zip, const std::string &entry, const std::string &baseDir,
               std::map<std::string, std::string> &rels)
{
    JZDomDocument doc;
    std::string err;
    if (!JZDocInternal::readXml(zip, entry, doc, err))
        return;

    std::vector<JZDomElement> list = JZDocInternal::childElements(doc.documentElement(), "Relationship");
    for (size_t i = 0; i < list.size(); i++)
    {
        std::string id = JZDocInternal::attrValue(list[i], "Id");
        std::string target = JZDocInternal::attrValue(list[i], "Target");
        if (id.empty() || target.empty())
            continue;
        // 外部链接(TargetMode="External")不进包, 跳过
        if (JZDocInternal::attrValue(list[i], "TargetMode") == "External")
            continue;
        rels[id] = JZDocInternal::resolveTarget(baseDir, target);
    }
}

// 段落引用的图片 rId(按出现顺序): <w:drawing> → <a:blip r:embed>; 旧式 <w:pict> → <v:imagedata r:id>
std::vector<std::string> paragraphImageIds(const JZDomElement &p)
{
    std::vector<std::string> ids;

    std::vector<JZDomElement> blips;
    JZDocInternal::findElements(p, "a:blip", blips);
    for (size_t i = 0; i < blips.size(); i++)
    {
        std::string rid = JZDocInternal::attrValue(blips[i], "embed");
        if (rid.empty())
            rid = JZDocInternal::attrValue(blips[i], "link");
        if (!rid.empty())
            ids.push_back(rid);
    }

    std::vector<JZDomElement> imagedata;
    JZDocInternal::findElements(p, "v:imagedata", imagedata);
    for (size_t i = 0; i < imagedata.size(); i++)
    {
        std::string rid = JZDocInternal::attrValue(imagedata[i], "id");
        if (!rid.empty())
            ids.push_back(rid);
    }
    return ids;
}

void parseParagraph(const JZDomElement &p, const std::map<std::string, DocxStyle> &styles,
                    JZDocSection &section)
{
    std::string text;
    JZDocInternal::collectParagraphText(p, text);

    JZDomElement pPr = JZDocInternal::childElement(p, "w:pPr");
    std::string styleId = JZDocInternal::attrValue(JZDocInternal::childElement(pPr, "w:pStyle"), "val");

    int level = 0;
    if (!styleId.empty())
    {
        std::map<std::string, DocxStyle>::const_iterator it = styles.find(styleId);
        if (it != styles.end())
        {
            if (it->second.outlineLvl >= 0)
                level = it->second.outlineLvl + 1;
            else
                level = headingLevelFromName(it->second.name);
        }
        else
        {
            level = headingLevelFromName(styleId);   // 退化: 只认 "Heading1" 这类 styleId
        }
    }

    std::string trimmed = JZDocInternal::trimmed(text);
    if (!trimmed.empty())
    {
        JZDocBlock block;
        block.type = JZDocBlockType::Text;
        block.text = text;
        block.style = styleId;
        block.level = level;
        section.blocks.push_back(block);
    }
}

void parseTable(const JZDomElement &tbl, JZDocSection &section)
{
    JZDocTable table;
    std::vector<JZDomElement> rows = JZDocInternal::childElements(tbl, "w:tr");
    for (size_t r = 0; r < rows.size(); r++)
    {
        std::vector<std::string> cells;
        std::vector<JZDomElement> tcs = JZDocInternal::childElements(rows[r], "w:tc");
        for (size_t c = 0; c < tcs.size(); c++)
        {
            std::string cell;
            std::vector<JZDomElement> ps = JZDocInternal::childElements(tcs[c], "w:p");
            for (size_t i = 0; i < ps.size(); i++)
            {
                std::string text;
                JZDocInternal::collectParagraphText(ps[i], text);
                if (!cell.empty())
                    cell += "\n";
                cell += text;
            }
            cells.push_back(cell);
        }
        table.rows.push_back(cells);
    }

    if (table.isEmpty())
        return;

    JZDocBlock block;
    block.type = JZDocBlockType::Table;
    block.style = "table";
    block.table = table;
    block.text = table.toMarkdown();
    section.blocks.push_back(block);
}

}  // namespace

JZResult<JZDocResult> jzReadDocx(const std::string &path)
{
    JZUnZip zip;
    JZResult<void> opened = zip.open(path);
    if (opened.isError())
        return JZResult<JZDocResult>::Error(opened.errorCode(), opened.errorMessage());

    JZDocResult result;
    result.format = "docx";

    JZDocSection section;
    section.name = "document";

    std::map<std::string, DocxStyle> styles;
    parseStyles(zip, styles);

    std::map<std::string, std::string> rels;
    parseRels(zip, "word/_rels/document.xml.rels", "word/", rels);

    JZDomDocument doc;
    std::string err;
    if (!JZDocInternal::readXml(zip, "word/document.xml", doc, err))
        return JZResult<JZDocResult>::Error("not a docx file: " + err);

    JZDomElement body = JZDocInternal::childElement(doc.documentElement(), "w:body");
    if (body.isNull())
        return JZResult<JZDocResult>::Error("not a docx file: word/document.xml has no <w:body>");

    std::map<std::string, int> imageCache;   // rId → images 下标
    std::vector<JZDomElement> children = JZDocInternal::childElements(body, "");
    for (size_t i = 0; i < children.size(); i++)
    {
        JZDomElement e = children[i];
        std::string tag = JZDocInternal::localName(e.tagName());

        if (tag == "tbl")
        {
            parseTable(e, section);
            continue;
        }
        if (tag != "p")
            continue;

        parseParagraph(e, styles, section);

        // 段落内的图片(<w:drawing> / <w:pict>), 按出现顺序插入, 位于段落文本之后
        std::vector<std::string> ids = paragraphImageIds(e);
        for (size_t k = 0; k < ids.size(); k++)
        {
            std::map<std::string, int>::iterator cached = imageCache.find(ids[k]);
            int index = -1;
            if (cached != imageCache.end())
            {
                index = cached->second;
            }
            else
            {
                std::map<std::string, std::string>::iterator target = rels.find(ids[k]);
                if (target == rels.end())
                    continue;   // 外部图片 / 关系缺失, 跳过

                JZResult<JZByteArray> data = zip.extractToMemory(target->second);
                if (data.isError())
                    continue;

                JZDocImage image;
                image.name = target->second;
                image.ext = JZDocInternal::fileExt(image.name);
                image.data = std::move(data.value());
                index = (int)result.images.size();
                result.images.push_back(image);
                imageCache[ids[k]] = index;
            }

            JZDocBlock block;
            block.type = JZDocBlockType::Image;
            block.image = index;
            block.style = "image";
            block.text = result.images[index].name;
            section.blocks.push_back(block);
        }
    }

    result.sections.push_back(section);

    // 标题: 首个标题块, 没有则取首段文本
    for (size_t i = 0; i < section.blocks.size(); i++)
    {
        const JZDocBlock &block = section.blocks[i];
        if (block.type == JZDocBlockType::Text && block.level > 0)
        {
            result.title = JZDocInternal::trimmed(block.text);
            break;
        }
    }
    if (result.title.empty() && !section.blocks.empty() && section.blocks[0].type == JZDocBlockType::Text)
        result.title = JZDocInternal::trimmed(section.blocks[0].text);

    return JZResult<JZDocResult>::Ok(result);
}
