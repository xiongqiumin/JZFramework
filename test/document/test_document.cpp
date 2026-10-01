// JZDocument 测试: 用 JZZip 现造最小 docx / xlsx(都是 zip + OOXML), 再交给 JZDocument 提取。
// 这样测试不依赖外部二进制素材, 改 XML 就能覆盖新场景。
#include "JZTest.h"
#include "JZApplication.h"
#include "JZDebug.h"

#include "JZDocument.h"
#include "JZCompress.h"
#include "JZFile.h"

#include <stdlib.h>

#include <string>
#include <utility>
#include <vector>

static const std::string kDocxPath = "jzdoc_test.docx";
static const std::string kXlsxPath = "jzdoc_test.xlsx";

// ---------------------------------------------------------------- 素材

static const char *kDocxDocument = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"
            xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"
            xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main">
  <w:body>
    <w:p><w:pPr><w:pStyle w:val="Heading1"/></w:pPr><w:r><w:t>测试标题</w:t></w:r></w:p>
    <w:p><w:r><w:t>第一段</w:t><w:tab/><w:t>制表符后</w:t><w:br/><w:t>换行后</w:t></w:r></w:p>
    <w:p><w:r><w:t>图片段落</w:t></w:r><w:r><w:drawing><a:blip r:embed="rId4"/></w:drawing></w:r></w:p>
    <w:p><w:r><w:t>同一张图再次引用</w:t></w:r><w:r><w:drawing><a:blip r:embed="rId4"/></w:drawing></w:r></w:p>
    <w:tbl>
      <w:tr>
        <w:tc><w:p><w:r><w:t>A1</w:t></w:r></w:p></w:tc>
        <w:tc><w:p><w:r><w:t>B1</w:t></w:r></w:p></w:tc>
      </w:tr>
      <w:tr>
        <w:tc><w:p><w:r><w:t>A2</w:t></w:r></w:p></w:tc>
        <w:tc><w:p><w:r><w:t>B2</w:t></w:r></w:p></w:tc>
      </w:tr>
    </w:tbl>
    <w:p><w:pPr><w:pStyle w:val="2"/></w:pPr><w:r><w:t>二级标题</w:t></w:r></w:p>
  </w:body>
</w:document>)";

static const char *kDocxStyles = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:styles xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">
  <w:style w:styleId="Heading1"><w:name w:val="heading 1"/><w:pPr><w:outlineLvl w:val="0"/></w:pPr></w:style>
  <w:style w:styleId="Heading2"><w:name w:val="heading 2"/><w:pPr><w:outlineLvl w:val="1"/></w:pPr></w:style>
  <w:style w:styleId="Normal"><w:name w:val="Normal"/></w:style>
  <!-- 中文 Word 常见写法: styleId 是数字, 只靠 outlineLvl 判层级 -->
  <w:style w:styleId="2"><w:name w:val="heading 2"/><w:pPr><w:outlineLvl w:val="1"/></w:pPr></w:style>
</w:styles>)";

static const char *kDocxRels = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId4" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image"
                Target="media/image1.png"/>
</Relationships>)";

static const char *kXlsxWorkbook = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"
          xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
  <sheets><sheet name="人员" sheetId="1" r:id="rId1"/></sheets>
</workbook>)";

static const char *kXlsxRels = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet"
                Target="worksheets/sheet1.xml"/>
</Relationships>)";

static const char *kXlsxShared = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<sst xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
  <si><t>姓名</t></si>
  <si><r><t>张</t></r><r><t>三</t></r></si>
</sst>)";

static const char *kXlsxStyles = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
  <numFmts count="1"><numFmt numFmtId="164" formatCode="yyyy-mm-dd"/></numFmts>
  <cellXfs count="2"><xf numFmtId="0"/><xf numFmtId="164"/></cellXfs>
</styleSheet>)";

// 列: A=共享串, B=数字/布尔/错误, C=内联串/小数
static const char *kXlsxSheet = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
  <sheetData>
    <row r="1"><c r="A1" t="s"><v>0</v></c><c r="B1" t="s"><v>1</v></c><c r="C1" t="inlineStr"><is><t>年龄</t></is></c></row>
    <row r="2"><c r="A2" t="s"><v>1</v></c><c r="B2"><v>18</v></c><c r="C2"><v>3.5</v></c></row>
    <row r="3"><c r="A3" s="1"><v>45000</v></c><c r="B3" t="b"><v>1</v></c><c r="C3" t="e"><v>#DIV/0!</v></c></row>
    <row r="4"/>
  </sheetData>
</worksheet>)";

// ---------------------------------------------------------------- 工具

static bool writeZip(const std::string &path, const std::vector<std::pair<std::string, std::string>> &files)
{
    JZZip zip;
    for (size_t i = 0; i < files.size(); i++)
        zip.addFile(files[i].first, JZByteArray(files[i].second.c_str(), (int)files[i].second.size()));

    JZResult<void> ret = zip.zip(path, 0);
    if (ret.isError())
        jzInfo() << "zip failed:" << ret.errorMessage();
    return ret.isOk();
}

static bool makeDocx(const std::string &path)
{
    std::vector<std::pair<std::string, std::string>> files;
    files.push_back(std::make_pair(std::string("[Content_Types].xml"),
                                   std::string("<?xml version=\"1.0\"?><Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"/>")));
    files.push_back(std::make_pair(std::string("word/document.xml"), std::string(kDocxDocument)));
    files.push_back(std::make_pair(std::string("word/styles.xml"), std::string(kDocxStyles)));
    files.push_back(std::make_pair(std::string("word/_rels/document.xml.rels"), std::string(kDocxRels)));
    files.push_back(std::make_pair(std::string("word/media/image1.png"), std::string("PNG-TEST-BYTES")));
    return writeZip(path, files);
}

static bool makeXlsx(const std::string &path)
{
    std::vector<std::pair<std::string, std::string>> files;
    files.push_back(std::make_pair(std::string("[Content_Types].xml"),
                                   std::string("<?xml version=\"1.0\"?><Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"/>")));
    files.push_back(std::make_pair(std::string("xl/workbook.xml"), std::string(kXlsxWorkbook)));
    files.push_back(std::make_pair(std::string("xl/_rels/workbook.xml.rels"), std::string(kXlsxRels)));
    files.push_back(std::make_pair(std::string("xl/sharedStrings.xml"), std::string(kXlsxShared)));
    files.push_back(std::make_pair(std::string("xl/styles.xml"), std::string(kXlsxStyles)));
    files.push_back(std::make_pair(std::string("xl/worksheets/sheet1.xml"), std::string(kXlsxSheet)));
    return writeZip(path, files);
}

// ---------------------------------------------------------------- 用例

// docx: 段落/标题层级/表格/内嵌图片(含 tab、br、同一图重复引用)
bool jzdoc_docx_extract()
{
    JZ_TEST_TRUE(makeDocx(kDocxPath));

    JZResult<JZDocResult> ret = JZDocument::open(kDocxPath);
    JZ_TEST_TRUE(ret.isOk());
    if (ret.isError())
        return false;
    JZDocResult doc = ret.value();

    JZ_TEST_EQ(std::string("docx"), doc.format);
    JZ_TEST_EQ(std::string("测试标题"), doc.title);
    JZ_TEST_EQ(1, (int)doc.sections.size());

    const std::vector<JZDocBlock> &blocks = doc.sections[0].blocks;
    JZ_TEST_EQ(8, (int)blocks.size());   // 标题 + 正文 + (文字,图片)x2 + 表格 + 二级标题

    // 标题
    JZ_TEST_TRUE(blocks[0].type == JZDocBlockType::Text);
    JZ_TEST_EQ(1, blocks[0].level);
    JZ_TEST_EQ(std::string("Heading1"), blocks[0].style);
    JZ_TEST_EQ(std::string("测试标题"), blocks[0].text);

    // 正文: tab / br 要保留成 \t / \n
    JZ_TEST_TRUE(blocks[1].type == JZDocBlockType::Text);
    JZ_TEST_EQ(0, blocks[1].level);
    JZ_TEST_EQ(std::string("第一段\t制表符后\n换行后"), blocks[1].text);

    // 图片(后面那个段落重复引用同一张图, 应复用同一个 images 下标)
    JZ_TEST_EQ(std::string("图片段落"), blocks[2].text);
    JZ_TEST_TRUE(blocks[3].type == JZDocBlockType::Image);
    JZ_TEST_EQ(std::string("同一张图再次引用"), blocks[4].text);
    JZ_TEST_TRUE(blocks[5].type == JZDocBlockType::Image);
    JZ_TEST_EQ(0, blocks[3].image);
    JZ_TEST_EQ(0, blocks[5].image);
    JZ_TEST_EQ(1, (int)doc.images.size());
    JZ_TEST_EQ(std::string("word/media/image1.png"), doc.images[0].name);
    JZ_TEST_EQ(std::string("png"), doc.images[0].ext);
    JZ_TEST_EQ(std::string("PNG-TEST-BYTES"), std::string(doc.images[0].data.constData(), doc.images[0].data.size()));

    // 表格
    JZ_TEST_TRUE(blocks[6].type == JZDocBlockType::Table);
    JZ_TEST_EQ(2, (int)blocks[6].table.rows.size());
    JZ_TEST_EQ(std::string("A1"), blocks[6].table.rows[0][0]);
    JZ_TEST_EQ(std::string("B1"), blocks[6].table.rows[0][1]);
    JZ_TEST_EQ(std::string("A2"), blocks[6].table.rows[1][0]);
    JZ_TEST_EQ(std::string("B2"), blocks[6].table.rows[1][1]);

    // 中文 Word 的数字 styleId(只靠 outlineLvl 判层级)
    JZ_TEST_TRUE(blocks[7].type == JZDocBlockType::Text);
    JZ_TEST_EQ(2, blocks[7].level);
    JZ_TEST_EQ(std::string("二级标题"), blocks[7].text);

    JZFile::remove(kDocxPath);
    return true;
}

// docx: text()/markdown()/toJson() 三种输出
bool jzdoc_docx_output()
{
    JZ_TEST_TRUE(makeDocx(kDocxPath));
    JZResult<JZDocResult> ret = JZDocument::open(kDocxPath);
    JZ_TEST_TRUE(ret.isOk());
    JZDocResult doc = ret.value();

    std::string text = doc.text();
    JZ_TEST_TRUE(text.find("# 测试标题") != std::string::npos);
    JZ_TEST_TRUE(text.find("第一段") != std::string::npos);
    JZ_TEST_TRUE(text.find("[图片: word/media/image1.png]") != std::string::npos);
    JZ_TEST_TRUE(text.find("| A1 | B1 |") != std::string::npos);

    std::string md = doc.markdown();
    JZ_TEST_TRUE(md.find("![word/media/image1.png]()") != std::string::npos);

    Json::Value json = doc.toJson();
    JZ_TEST_EQ(std::string("docx"), json["format"].asString());
    JZ_TEST_EQ(8, (int)json["sections"][0]["blocks"].size());
    JZ_TEST_EQ(std::string("text"), json["sections"][0]["blocks"][0]["type"].asString());
    JZ_TEST_EQ(1, json["sections"][0]["blocks"][0]["level"].asInt());
    JZ_TEST_EQ(std::string("image"), json["sections"][0]["blocks"][3]["type"].asString());
    JZ_TEST_EQ(std::string("table"), json["sections"][0]["blocks"][6]["type"].asString());
    JZ_TEST_EQ(std::string("A1"), json["sections"][0]["blocks"][6]["rows"][0][0].asString());
    JZ_TEST_EQ(1, (int)json["images"].size());
    JZ_TEST_TRUE(json["images"][0]["data"].isNull());   // 默认不带 base64

    // "PNG-TEST-BYTES" 的 base64
    Json::Value jsonWithData = doc.toJson(true);
    JZ_TEST_EQ(std::string("UE5HLVRFU1QtQllURVM="), jsonWithData["images"][0]["data"].asString());

    JZFile::remove(kDocxPath);
    return true;
}

// xlsx: 共享串/内联串/数字/日期/布尔/错误值, 空行丢弃
bool jzdoc_xlsx_extract()
{
    JZ_TEST_TRUE(makeXlsx(kXlsxPath));

    JZResult<JZDocResult> ret = JZDocument::open(kXlsxPath);
    JZ_TEST_TRUE(ret.isOk());
    if (ret.isError())
        return false;
    JZDocResult doc = ret.value();

    JZ_TEST_EQ(std::string("xlsx"), doc.format);
    JZ_TEST_EQ(1, (int)doc.sections.size());
    JZ_TEST_EQ(std::string("人员"), doc.sections[0].name);
    JZ_TEST_EQ(std::string("人员"), doc.title);

    const std::vector<JZDocBlock> &blocks = doc.sections[0].blocks;
    JZ_TEST_EQ(1, (int)blocks.size());                       // 一个 sheet → 一个 Table 块
    JZ_TEST_TRUE(blocks[0].type == JZDocBlockType::Table);

    const std::vector<std::vector<std::string>> &rows = blocks[0].table.rows;
    JZ_TEST_EQ(3, (int)rows.size());                         // 第 4 行是空行, 被丢掉

    JZ_TEST_EQ(3, (int)rows[0].size());
    JZ_TEST_EQ(std::string("姓名"), rows[0][0]);             // t="s" 共享串
    JZ_TEST_EQ(std::string("张三"), rows[0][1]);             // 富文本多 run 拼接
    JZ_TEST_EQ(std::string("年龄"), rows[0][2]);             // t="inlineStr"

    JZ_TEST_EQ(std::string("18"), rows[1][1]);               // 整数不带小数点
    JZ_TEST_EQ(std::string("3.5"), rows[1][2]);              // 小数

    JZ_TEST_EQ(std::string("2023-03-15"), rows[2][0]);       // s="1" → numFmt 164 → 日期
    JZ_TEST_EQ(std::string("TRUE"), rows[2][1]);             // t="b"
    JZ_TEST_EQ(std::string("#DIV/0!"), rows[2][2]);          // t="e"

    JZ_TEST_TRUE(blocks[0].text.find("| 姓名 | 张三 | 年龄 |") != std::string::npos);
    jzInfo() << "xlsx markdown:\n" << doc.markdown();

    JZFile::remove(kXlsxPath);
    return true;
}

// 门面: 按后缀分派 + 错误路径
bool jzdoc_facade()
{
    JZ_TEST_TRUE(JZDocument::formatOfFile("a.docx") == JZDocument::FormatDocx);
    JZ_TEST_TRUE(JZDocument::formatOfFile("a.xlsx") == JZDocument::FormatXlsx);
    JZ_TEST_TRUE(JZDocument::formatOfFile("a.pdf") == JZDocument::FormatPdf);
    JZ_TEST_TRUE(JZDocument::formatOfFile("a.txt") == JZDocument::FormatUnknown);

    std::vector<std::string> formats = JZDocument::supportedFormats();
    JZ_TEST_EQ(2, (int)formats.size());
    JZ_TEST_EQ(std::string("docx"), formats[0]);
    JZ_TEST_EQ(std::string("xlsx"), formats[1]);

    // 文件不存在
    JZ_TEST_TRUE(JZDocument::open("no_such_file.docx").isError());
    // 格式不支持
    JZ_TEST_TRUE(JZDocument::open("no_such_file.txt").isError());
    // pdf 后端还没接
    JZ_TEST_TRUE(JZDocument::open("x.pdf", JZDocument::FormatPdf).isError());

    // zip 里没有 word/document.xml → 明确报错, 而不是返回空结果
    JZ_TEST_TRUE(makeXlsx(kDocxPath));
    JZ_TEST_TRUE(JZDocument::open(kDocxPath, JZDocument::FormatDocx).isError());
    JZFile::remove(kDocxPath);
    return true;
}

// 真实文件冒烟: 设环境变量 JZDOC_SMOKE_FILE 指向真实 docx/xlsx(默认不跑, 因为仓库里没有素材)。
// 只打印结构统计, 不打印正文 —— 既能验证「真 Office 文件能不能解析」, 又不泄露文档内容。
bool jzdoc_smoke_real_file()
{
    const char *path = getenv("JZDOC_SMOKE_FILE");
    if (!path || !*path)
    {
        jzInfo() << "skip: JZDOC_SMOKE_FILE not set";
        return true;
    }

    JZResult<JZDocResult> ret = JZDocument::open(path);
    if (ret.isError())
    {
        jzInfo() << "open failed:" << ret.errorMessage();
        return false;
    }
    JZ_TEST_TRUE(ret.isOk());

    const JZDocResult &doc = ret.value();
    int textBlocks = 0, imageBlocks = 0, tableBlocks = 0, tableRows = 0;
    for (size_t s = 0; s < doc.sections.size(); s++)
    {
        for (size_t i = 0; i < doc.sections[s].blocks.size(); i++)
        {
            const JZDocBlock &block = doc.sections[s].blocks[i];
            if (block.type == JZDocBlockType::Text)
                textBlocks++;
            else if (block.type == JZDocBlockType::Image)
                imageBlocks++;
            else if (block.type == JZDocBlockType::Table)
            {
                tableBlocks++;
                tableRows += block.table.rowCount();
            }
        }
    }
    jzInfo() << "smoke" << path
             << "format:" << doc.format
             << "sections:" << (int)doc.sections.size()
             << "text:" << textBlocks
             << "image:" << imageBlocks
             << "table:" << tableBlocks
             << "rows:" << tableRows
             << "images:" << (int)doc.images.size()
             << "textLen:" << (int)doc.text().size();
    JZ_TEST_GT(textBlocks + imageBlocks + tableBlocks, 0);
    return true;
}

// PDF 后端(jzpdf/PDFium): 设 JZDOC_PDF_FILE 指向真实 pdf(默认跳过, 仓库里没有素材)。
// 只打印结构统计, 不打印正文。
bool jzdoc_pdf_extract()
{
    const char *path = getenv("JZDOC_PDF_FILE");
    if (!path || !*path)
    {
        jzInfo() << "skip: JZDOC_PDF_FILE not set";
        return true;
    }

    JZResult<JZDocResult> ret = JZDocument::open(path);
    if (ret.isError())
    {
        jzInfo() << "open failed:" << ret.errorMessage();
        return false;
    }
    JZ_TEST_TRUE(ret.isOk());

    const JZDocResult &doc = ret.value();
    JZ_TEST_EQ(std::string("pdf"), doc.format);
    JZ_TEST_GT((int)doc.sections.size(), 0);

    int textBlocks = 0, imageBlocks = 0;
    int textLen = 0;
    bool hasCjk = false;
    for (size_t s = 0; s < doc.sections.size(); s++)
    {
        for (size_t i = 0; i < doc.sections[s].blocks.size(); i++)
        {
            const JZDocBlock &block = doc.sections[s].blocks[i];
            if (block.type == JZDocBlockType::Text)
            {
                textBlocks++;
                textLen += (int)block.text.size();
                // 文本块必须有 bbox(PDF 的核心价值就是带坐标)
                JZ_TEST_TRUE(block.bbox.w > 0 && block.bbox.h > 0);
                // 中文 PDF: 确认传到 C++ 侧的是真 UTF-8(没被 \uXXXX 转义后没解回来)
                for (size_t k = 0; k < block.text.size(); k++)
                {
                    if ((unsigned char)block.text[k] >= 0xE4)
                        hasCjk = true;
                }
            }
            else if (block.type == JZDocBlockType::Image)
            {
                imageBlocks++;
                JZ_TEST_TRUE(block.image >= 0 && block.image < (int)doc.images.size());
                JZ_TEST_TRUE(doc.images[block.image].data.size() > 0);
            }
        }
    }
    jzInfo() << "pdf" << path
             << "pages:" << (int)doc.sections.size()
             << "text:" << textBlocks
             << "image:" << imageBlocks
             << "images:" << (int)doc.images.size()
             << "textLen:" << textLen
             << "cjk:" << (int)(hasCjk ? 1 : 0);
    JZ_TEST_GT(textBlocks + imageBlocks, 0);
    JZ_TEST_GT(textLen, 0);
    JZ_TEST_TRUE(hasCjk);
    return true;
}

int main(int argc, char *argv[])
{
    JZApplication app;

    JZTest test;
    auto suite = std::make_shared<JZTestSuite>("document");
    suite->addTestCase("docx_extract", jzdoc_docx_extract);
    suite->addTestCase("docx_output", jzdoc_docx_output);
    suite->addTestCase("xlsx_extract", jzdoc_xlsx_extract);
    suite->addTestCase("facade", jzdoc_facade);
    suite->addTestCase("smoke_real_file", jzdoc_smoke_real_file);
    suite->addTestCase("pdf_extract", jzdoc_pdf_extract);
    test.addSuite(suite);

    return test.run(argc, argv);
}
