#ifndef JZ_DOCUMENT_H_
#define JZ_DOCUMENT_H_

#include <string>
#include <vector>

#include "JZCore/JZByteArray.h"
#include "JZCore/JZResult.h"
#include "JZJson/JZJson.h"

// ============================================================================
// JZDocument —— 办公文档「信息提取」模块(只读)
//
// 目标: 把 docx / xlsx / pdf 里**能给 LLM 用**的信息抽出来 —— 文本(带层级/样式)、
//       表格(结构化单元格)、内嵌原图(原始字节, 不解码)。不负责排版渲染, 不负责写回。
//
// 统一产出物是 JZDocResult:
//     format   —— "docx" / "xlsx" / "pdf"
//     sections —— 逻辑分段: docx=正文(单段), xlsx=每个 sheet 一段, pdf=每页一段
//     blocks   —— 段内的块, 有序(按文档出现顺序), 三种:
//                 Text  : 段落文本 / 标题(带 level) / xlsx 无
//                 Image : 内嵌图片, 指向 images[block.image]
//                 Table : 表格, 文本是 markdown 表格, 结构化数据在 block.table.rows
//     images   —— 提取出的内嵌图片原始字节(如 docx 的 word/media/*)
//
// 输出到 LLM 的三种形态:
//     result.text()           纯文本 / 轻量 markdown(标题层级用 # 表示)
//     result.markdown()       markdown(图片为 ![]())
//     result.toJson()         结构化 JSON(默认图片只给元信息, withImageData=true 带 base64)
//
// 依赖: JZXml(OOXML 是 XML) + JZCompress(docx/xlsx 本质是 zip) + JZJson(输出)。
//       没有引入任何第三方文档库, 全部自研解析。
// ============================================================================

// 块类型
enum class JZFRAMEWORK_API JZDocBlockType
{
    Text,    // 文本 / 标题
    Image,   // 内嵌图片
    Table,   // 表格
};

// 页面 / 画布坐标(PDF 用; docx/xlsx 无布局信息, 恒为 0)
struct JZFRAMEWORK_API JZDocRect
{
    double x = 0;
    double y = 0;
    double w = 0;
    double h = 0;
};

// 提取出的内嵌图片(原始字节, 不解码)
struct JZFRAMEWORK_API JZDocImage
{
    std::string name;    // 包内路径, 如 "word/media/image1.png"
    std::string ext;     // 后缀(小写, 不含点): png / jpeg / emf ...
    JZByteArray data;    // 原始字节
};

// 结构化表格
struct JZFRAMEWORK_API JZDocTable
{
    std::vector<std::vector<std::string>> rows;

    bool isEmpty() const;
    int rowCount() const;
    // 首行当表头, 生成 markdown 表格
    std::string toMarkdown() const;
};

// 一个内容块
struct JZFRAMEWORK_API JZDocBlock
{
    JZDocBlockType type = JZDocBlockType::Text;

    std::string text;    // Text: 文本; Table: markdown 表格(同 table.toMarkdown())
    std::string style;   // 样式名(docx 的 pStyle; xlsx 为 "sheet")
    int level = 0;       // 标题级别 1..9; 0 = 正文
    JZDocRect bbox;      // PDF 页面坐标; docx/xlsx 恒 0

    int image = -1;      // type == Image 时, 指向所属 JZDocResult::images 的下标
    JZDocTable table;    // type == Table 时的结构化单元格
};

// 一个逻辑分段(sheet / 页 / 正文)
struct JZFRAMEWORK_API JZDocSection
{
    std::string name;    // sheet 名 / "page 1" / "document"
    std::vector<JZDocBlock> blocks;
};

// 解析结果
struct JZFRAMEWORK_API JZDocResult
{
    std::string format;                 // "docx" / "xlsx" / "pdf"
    std::string title;                  // 标题: docx=首个标题, xlsx=工作簿名(首个 sheet 名), pdf=元数据 title
    std::vector<JZDocSection> sections;
    std::vector<JZDocImage> images;

    bool isEmpty() const;
    int blockCount() const;

    // 纯文本(标题带 # 层级, 图片占位, 表转 markdown)。可直接喂 LLM。
    std::string text() const;
    // markdown(图片用 ![]())
    std::string markdown() const;
    // 结构化 JSON。withImageData=true 时图片带 base64(体积会变大, 默认只给元信息)。
    Json::Value toJson(bool withImageData = false) const;

    // 反向: 从同构 JSON 还原(toJson 的逆, 也是外部 CLI 后端的接入点)。
    // 图片: 有 "data" 就 base64 解码; 有 "path" 就按路径读文件(读不到只留元信息, 不失败)。
    static JZResult<JZDocResult> fromJson(const Json::Value &root);
};

// ============================================================================
// JZDocument —— 门面: 按扩展名分派到具体后端
// ============================================================================
class JZFRAMEWORK_API JZDocument
{
public:
    enum Format
    {
        FormatUnknown,
        FormatDocx,
        FormatXlsx,
        FormatPdf,   // 尚未接入: PDF 走外部单二进制 CLI(mutool 等), 见模块设计说明
    };

    // 按文件后缀判定格式(不读内容)
    static Format formatOfFile(const std::string &path);
    // 当前已实现的后端后缀: {"docx", "xlsx"}
    static std::vector<std::string> supportedFormats();

    // 打开并提取。失败时 JZResult 带错误信息(文件不存在 / 不是 zip / 缺关键条目 / XML 解析失败)。
    static JZResult<JZDocResult> open(const std::string &path);
    static JZResult<JZDocResult> open(const std::string &path, Format format);
};

// 单独入口: 门面内部就是按格式分派到这两个函数, 也可直接调用
JZFRAMEWORK_API JZResult<JZDocResult> jzReadDocx(const std::string &path);
JZFRAMEWORK_API JZResult<JZDocResult> jzReadXlsx(const std::string &path);
// PDF: 走外部单二进制 CLI(jzpdf, PDFium 内核), 见 JZPdfBackend.cpp / binary/jzpdf
JZFRAMEWORK_API JZResult<JZDocResult> jzReadPdf(const std::string &path);

#endif // !JZ_DOCUMENT_H_
