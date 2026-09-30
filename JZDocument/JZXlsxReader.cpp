#include "JZDocument.h"
#include "JZDocumentInternal.h"

#include "JZCore/JZStd.h"

// ============================================================================
// xlsx 提取
//
// xlsx = zip + OOXML(SpreadsheetML)。关注:
//   xl/workbook.xml                 sheet 列表(name + r:id)
//   xl/_rels/workbook.xml.rels      r:id → worksheet 路径
//   xl/worksheets/sheetN.xml        <sheetData> → <row> → <c>
//   xl/sharedStrings.xml            共享字符串(<si>), 单元格 t="s" 时按 <v> 索引
//   xl/styles.xml                   cellXfs/numFmts —— 用来判断「这个数字其实是日期」
//
// 单元格类型 t: s=共享字符串, inlineStr=内联, str=公式字符串结果, b=布尔, e=错误,
//               d=ISO 日期, n/缺省=数字。数字按 numFmt 判定是否转成日期字符串。
//
// 每个 sheet 输出一个 Table 块: text 是 markdown 表格(首行当表头), table.rows 是结构化单元格。
// 空行(整行都没有值)默认丢掉, 免得几万空行灌给 LLM。
// 注: xlsx 里的图片(xl/media/*)多是图表/装饰, 且没有单元格级锚点信息, 这里不抽。
// ============================================================================

namespace
{
struct SheetInfo
{
    std::string name;
    std::string target;   // zip 内路径
};

// custom numFmt: id → formatCode
void parseNumFmts(JZUnZip &zip, std::map<int, std::string> &numFmts)
{
    JZDomDocument doc;
    std::string err;
    if (!JZDocInternal::readXml(zip, "xl/styles.xml", doc, err))
        return;

    std::vector<JZDomElement> list = JZDocInternal::childElements(doc.documentElement(), "numFmts");
    for (size_t i = 0; i < list.size(); i++)
    {
        std::vector<JZDomElement> fmts = JZDocInternal::childElements(list[i], "numFmt");
        for (size_t k = 0; k < fmts.size(); k++)
        {
            bool ok = false;
            int id = str_toInt(JZDocInternal::attrValue(fmts[k], "numFmtId"), &ok);
            if (ok)
                numFmts[id] = JZDocInternal::attrValue(fmts[k], "formatCode");
        }
    }
}

// cellXfs: 第 i 个 <xf> 的 numFmtId(单元格 s="i" 指向它)
void parseCellXfs(JZUnZip &zip, std::vector<int> &cellXfs)
{
    JZDomDocument doc;
    std::string err;
    if (!JZDocInternal::readXml(zip, "xl/styles.xml", doc, err))
        return;

    JZDomElement xfs = JZDocInternal::childElement(doc.documentElement(), "cellXfs");
    std::vector<JZDomElement> list = JZDocInternal::childElements(xfs, "xf");
    for (size_t i = 0; i < list.size(); i++)
    {
        bool ok = false;
        int id = str_toInt(JZDocInternal::attrValue(list[i], "numFmtId"), &ok);
        cellXfs.push_back(ok ? id : 0);
    }
}

void parseSharedStrings(JZUnZip &zip, std::vector<std::string> &sharedStrings)
{
    JZDomDocument doc;
    std::string err;
    if (!JZDocInternal::readXml(zip, "xl/sharedStrings.xml", doc, err))
        return;   // 没有共享字符串表: 全是数字/内联串, 合法

    std::vector<JZDomElement> list = JZDocInternal::childElements(doc.documentElement(), "si");
    for (size_t i = 0; i < list.size(); i++)
    {
        std::string text;
        JZDocInternal::collectTextNodes(list[i], text);
        sharedStrings.push_back(text);
    }
}

std::string cellValue(const JZDomElement &c, const std::vector<std::string> &sharedStrings,
                      const std::vector<int> &cellXfs, const std::map<int, std::string> &numFmts)
{
    std::string type = JZDocInternal::attrValue(c, "t");
    JZDomElement v = JZDocInternal::childElement(c, "v");
    std::string raw = v.isNull() ? std::string() : v.text();

    if (type == "inlineStr")
    {
        std::string text;
        JZDomElement is = JZDocInternal::childElement(c, "is");
        JZDocInternal::collectTextNodes(is.isNull() ? c : is, text);
        return text;
    }
    if (type == "s")
    {
        bool ok = false;
        int index = str_toInt(raw, &ok);
        if (!ok || index < 0 || index >= (int)sharedStrings.size())
            return std::string();
        return sharedStrings[index];
    }
    if (type == "str" || type == "e" || type == "d")
        return raw;                      // 公式字符串结果 / 错误值 / ISO 日期
    if (type == "b")
        return (raw == "1" || raw == "true" || raw == "TRUE") ? "TRUE" : "FALSE";

    // 数字(含 t 缺省 / t="n")
    bool ok = false;
    double number = str_toDouble(raw, &ok);
    if (!ok)
        return raw;

    bool styleOk = false;
    int styleIndex = str_toInt(JZDocInternal::attrValue(c, "s"), &styleOk);
    if (styleOk && styleIndex >= 0 && styleIndex < (int)cellXfs.size())
    {
        int numFmtId = cellXfs[styleIndex];
        std::map<int, std::string>::const_iterator it = numFmts.find(numFmtId);
        std::string code = (it != numFmts.end()) ? it->second : std::string();
        if (JZDocInternal::isDateNumFmt(numFmtId, code))
        {
            std::string date = JZDocInternal::serialToDate(number);
            if (!date.empty())
                return date;
        }
    }
    return JZDocInternal::formatNumber(number);
}

bool rowIsEmpty(const std::vector<std::string> &cells)
{
    for (size_t i = 0; i < cells.size(); i++)
    {
        if (!JZDocInternal::trimmed(cells[i]).empty())
            return false;
    }
    return true;
}

void parseSheet(JZUnZip &zip, const SheetInfo &sheet, const std::vector<std::string> &sharedStrings,
                const std::vector<int> &cellXfs, const std::map<int, std::string> &numFmts,
                JZDocResult &result)
{
    JZDomDocument doc;
    std::string err;
    if (!JZDocInternal::readXml(zip, sheet.target, doc, err))
        return;   // 单个 sheet 读不出来, 跳过而不是整篇失败

    JZDomElement sheetData = JZDocInternal::childElement(doc.documentElement(), "sheetData");
    if (sheetData.isNull())
        return;

    JZDocSection section;
    section.name = sheet.name;

    JZDocTable table;
    std::vector<JZDomElement> rows = JZDocInternal::childElements(sheetData, "row");
    for (size_t r = 0; r < rows.size(); r++)
    {
        std::vector<std::string> cells;
        int col = 0;
        std::vector<JZDomElement> cs = JZDocInternal::childElements(rows[r], "c");
        for (size_t k = 0; k < cs.size(); k++)
        {
            int index = JZDocInternal::columnFromRef(JZDocInternal::attrValue(cs[k], "r"));
            if (index >= 0)
                col = index;
            if ((int)cells.size() < col)
                cells.resize(col, std::string());

            std::string value = cellValue(cs[k], sharedStrings, cellXfs, numFmts);
            if (col == (int)cells.size())
                cells.push_back(value);
            else
                cells[col] = value;
            col++;
        }

        // 去掉尾部空单元格(很多表右侧有一堆空列)
        while (!cells.empty() && cells.back().empty())
            cells.pop_back();
        if (cells.empty() || rowIsEmpty(cells))
            continue;

        table.rows.push_back(cells);
    }

    if (table.isEmpty())
        return;   // 空 sheet 不产块

    JZDocBlock block;
    block.type = JZDocBlockType::Table;
    block.style = "sheet";
    block.table = table;
    block.text = table.toMarkdown();
    section.blocks.push_back(block);
    result.sections.push_back(section);
}

}  // namespace

JZResult<JZDocResult> jzReadXlsx(const std::string &path)
{
    JZUnZip zip;
    JZResult<void> opened = zip.open(path);
    if (opened.isError())
        return JZResult<JZDocResult>::Error(opened.errorCode(), opened.errorMessage());

    JZDocResult result;
    result.format = "xlsx";

    std::map<std::string, std::string> rels;
    // 关系表里 Target 相对 xl/ 目录
    {
        JZDomDocument relDoc;
        std::string err;
        if (JZDocInternal::readXml(zip, "xl/_rels/workbook.xml.rels", relDoc, err))
        {
            std::vector<JZDomElement> list = JZDocInternal::childElements(relDoc.documentElement(), "Relationship");
            for (size_t i = 0; i < list.size(); i++)
            {
                std::string id = JZDocInternal::attrValue(list[i], "Id");
                std::string target = JZDocInternal::attrValue(list[i], "Target");
                if (id.empty() || target.empty() || JZDocInternal::attrValue(list[i], "TargetMode") == "External")
                    continue;
                rels[id] = JZDocInternal::resolveTarget("xl/", target);
            }
        }
    }

    JZDomDocument workbook;
    std::string err;
    if (!JZDocInternal::readXml(zip, "xl/workbook.xml", workbook, err))
        return JZResult<JZDocResult>::Error("not a xlsx file: " + err);

    JZDomElement sheets = JZDocInternal::childElement(workbook.documentElement(), "sheets");
    std::vector<JZDomElement> sheetList = JZDocInternal::childElements(sheets, "sheet");
    if (sheetList.empty())
        return JZResult<JZDocResult>::Error("not a xlsx file: xl/workbook.xml has no <sheet>");

    std::vector<SheetInfo> infos;
    for (size_t i = 0; i < sheetList.size(); i++)
    {
        SheetInfo info;
        info.name = JZDocInternal::attrValue(sheetList[i], "name");
        std::string rid = JZDocInternal::attrValue(sheetList[i], "r:id");
        std::map<std::string, std::string>::iterator it = rels.find(rid);
        if (it != rels.end())
            info.target = it->second;
        else
            info.target = str_asprintf("xl/worksheets/sheet%d.xml", (int)i + 1);   // 关系表缺失时的兜底
        infos.push_back(info);
    }

    std::vector<std::string> sharedStrings;
    parseSharedStrings(zip, sharedStrings);

    std::vector<int> cellXfs;
    parseCellXfs(zip, cellXfs);

    std::map<int, std::string> numFmts;
    parseNumFmts(zip, numFmts);

    for (size_t i = 0; i < infos.size(); i++)
        parseSheet(zip, infos[i], sharedStrings, cellXfs, numFmts, result);

    if (result.title.empty() && !result.sections.empty())
        result.title = result.sections[0].name;   // 工作簿名拿不到时, 用首个 sheet 名
    return JZResult<JZDocResult>::Ok(result);
}
