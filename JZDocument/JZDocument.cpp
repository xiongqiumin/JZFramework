#include "JZDocument.h"

#include "JZDocumentInternal.h"

#include "JZCore/JZFile.h"
#include "JZCore/JZFileInfo.h"
#include "JZCore/JZStd.h"

// ============================================================================
// JZDocResult 的三种输出形态 + JZDocument 门面
// ============================================================================

namespace
{
const char *blockTypeName(JZDocBlockType type)
{
    switch (type)
    {
    case JZDocBlockType::Text:  return "text";
    case JZDocBlockType::Image: return "image";
    case JZDocBlockType::Table: return "table";
    }
    return "text";
}

std::string headingMarks(int level)
{
    if (level <= 0)
        return std::string();
    if (level > 6)
        level = 6;
    return std::string(level, '#') + " ";
}

std::string base64Of(const JZByteArray &data)
{
    JZByteArray encoded = data.toBase64();
    if (encoded.isEmpty())
        return std::string();
    std::string out(encoded.constData(), (size_t)encoded.size());
    // toBase64() 可能带一个尾部 '\0'(取决于 outlen 是否含结束符), 去掉再进 JSON
    while (!out.empty() && out.back() == '\0')
        out.pop_back();
    return out;
}
}  // namespace

// ---------------- JZDocTable ----------------
bool JZDocTable::isEmpty() const
{
    return rows.empty();
}

int JZDocTable::rowCount() const
{
    return (int)rows.size();
}

std::string JZDocTable::toMarkdown() const
{
    if (rows.empty())
        return std::string();

    std::string out;
    const std::vector<std::string> &header = rows[0];

    out += "|";
    for (size_t i = 0; i < header.size(); i++)
        out += " " + JZDocInternal::markdownCell(header[i]) + " |";
    out += "\n|";
    for (size_t i = 0; i < header.size(); i++)
        out += " --- |";
    out += "\n";

    for (size_t r = 1; r < rows.size(); r++)
    {
        out += "|";
        for (size_t c = 0; c < rows[r].size(); c++)
            out += " " + JZDocInternal::markdownCell(rows[r][c]) + " |";
        out += "\n";
    }
    return out;
}

// ---------------- JZDocResult ----------------
bool JZDocResult::isEmpty() const
{
    for (size_t i = 0; i < sections.size(); i++)
    {
        if (!sections[i].blocks.empty())
            return false;
    }
    return true;
}

int JZDocResult::blockCount() const
{
    int count = 0;
    for (size_t i = 0; i < sections.size(); i++)
        count += (int)sections[i].blocks.size();
    return count;
}

std::string JZDocResult::text() const
{
    std::string out;
    for (size_t s = 0; s < sections.size(); s++)
    {
        const JZDocSection &sec = sections[s];
        bool needHeader = !sec.name.empty() && (sections.size() > 1 || sec.name != "document");
        if (needHeader)
            out += "## " + sec.name + "\n\n";

        for (size_t i = 0; i < sec.blocks.size(); i++)
        {
            const JZDocBlock &block = sec.blocks[i];
            if (block.type == JZDocBlockType::Text)
            {
                out += headingMarks(block.level) + JZDocInternal::trimmedRight(block.text) + "\n\n";
            }
            else if (block.type == JZDocBlockType::Image)
            {
                std::string name = block.text;
                if (block.image >= 0 && block.image < (int)images.size())
                    name = images[block.image].name;
                out += "[图片: " + name + "]\n\n";
            }
            else if (block.type == JZDocBlockType::Table)
            {
                out += block.text + "\n\n";
            }
        }
    }
    return out;
}

std::string JZDocResult::markdown() const
{
    std::string out;
    if (!title.empty())
        out += "# " + title + "\n\n";

    for (size_t s = 0; s < sections.size(); s++)
    {
        const JZDocSection &sec = sections[s];
        bool needHeader = !sec.name.empty() && (sections.size() > 1 || sec.name != "document");
        if (needHeader)
            out += "## " + sec.name + "\n\n";

        for (size_t i = 0; i < sec.blocks.size(); i++)
        {
            const JZDocBlock &block = sec.blocks[i];
            if (block.type == JZDocBlockType::Text)
            {
                out += headingMarks(block.level) + JZDocInternal::trimmedRight(block.text) + "\n\n";
            }
            else if (block.type == JZDocBlockType::Image)
            {
                std::string name = block.text;
                if (block.image >= 0 && block.image < (int)images.size())
                    name = images[block.image].name;
                out += "![" + name + "]()\n\n";
            }
            else if (block.type == JZDocBlockType::Table)
            {
                out += block.text + "\n\n";
            }
        }
    }
    return out;
}

Json::Value JZDocResult::toJson(bool withImageData) const
{
    Json::Value root;
    root["format"] = format;
    root["title"] = title;

    Json::Value sectionsJson(Json::arrayValue);
    for (size_t s = 0; s < sections.size(); s++)
    {
        const JZDocSection &sec = sections[s];
        Json::Value secJson;
        secJson["name"] = sec.name;

        Json::Value blocksJson(Json::arrayValue);
        for (size_t i = 0; i < sec.blocks.size(); i++)
        {
            const JZDocBlock &block = sec.blocks[i];
            Json::Value blockJson;
            blockJson["type"] = blockTypeName(block.type);
            if (!block.style.empty())
                blockJson["style"] = block.style;
            if (block.level > 0)
                blockJson["level"] = block.level;

            if (block.type == JZDocBlockType::Image)
            {
                blockJson["image"] = block.image;
                if (block.image >= 0 && block.image < (int)images.size())
                    blockJson["name"] = images[block.image].name;
            }
            else if (block.type == JZDocBlockType::Table)
            {
                Json::Value rowsJson(Json::arrayValue);
                for (size_t r = 0; r < block.table.rows.size(); r++)
                {
                    Json::Value rowJson(Json::arrayValue);
                    for (size_t c = 0; c < block.table.rows[r].size(); c++)
                        rowJson.append(block.table.rows[r][c]);
                    rowsJson.append(rowJson);
                }
                blockJson["rows"] = rowsJson;
                blockJson["markdown"] = block.text;
            }
            else
            {
                blockJson["text"] = block.text;
            }

            // PDF 才有 bbox, docx/xlsx 全 0 时不输出, 免得噪声
            if (block.bbox.w > 0 || block.bbox.h > 0)
            {
                blockJson["bbox"] = Json::Value(Json::arrayValue);
                blockJson["bbox"].append(block.bbox.x);
                blockJson["bbox"].append(block.bbox.y);
                blockJson["bbox"].append(block.bbox.w);
                blockJson["bbox"].append(block.bbox.h);
            }
            blocksJson.append(blockJson);
        }
        secJson["blocks"] = blocksJson;
        sectionsJson.append(secJson);
    }
    root["sections"] = sectionsJson;

    Json::Value imagesJson(Json::arrayValue);
    for (size_t i = 0; i < images.size(); i++)
    {
        Json::Value imgJson;
        imgJson["name"] = images[i].name;
        imgJson["ext"] = images[i].ext;
        imgJson["size"] = images[i].data.size();
        if (withImageData)
            imgJson["data"] = base64Of(images[i].data);
        imagesJson.append(imgJson);
    }
    root["images"] = imagesJson;

    return root;
}

JZResult<JZDocResult> JZDocResult::fromJson(const Json::Value &root)
{
    JZDocResult doc;
    doc.format = root.isMember("format") ? root["format"].asString() : std::string();
    doc.title = root.isMember("title") ? root["title"].asString() : std::string();

    const Json::Value &sectionsJson = root["sections"];
    for (Json::ArrayIndex s = 0; s < sectionsJson.size(); s++)
    {
        JZDocSection sec;
        sec.name = sectionsJson[s].isMember("name") ? sectionsJson[s]["name"].asString() : std::string();

        const Json::Value &blocksJson = sectionsJson[s]["blocks"];
        for (Json::ArrayIndex i = 0; i < blocksJson.size(); i++)
        {
            const Json::Value &b = blocksJson[i];
            JZDocBlock block;
            std::string type = b.isMember("type") ? b["type"].asString() : std::string();
            if (type == "image")
                block.type = JZDocBlockType::Image;
            else if (type == "table")
                block.type = JZDocBlockType::Table;
            else
                block.type = JZDocBlockType::Text;

            if (b.isMember("style"))
                block.style = b["style"].asString();
            if (b.isMember("level"))
                block.level = b["level"].asInt();

            if (b.isMember("bbox") && b["bbox"].size() == 4)
            {
                block.bbox.x = b["bbox"][0].asDouble();
                block.bbox.y = b["bbox"][1].asDouble();
                block.bbox.w = b["bbox"][2].asDouble();
                block.bbox.h = b["bbox"][3].asDouble();
            }

            if (block.type == JZDocBlockType::Table)
            {
                const Json::Value &rowsJson = b["rows"];
                for (Json::ArrayIndex r = 0; r < rowsJson.size(); r++)
                {
                    std::vector<std::string> row;
                    for (Json::ArrayIndex c = 0; c < rowsJson[r].size(); c++)
                        row.push_back(rowsJson[r][c].asString());
                    block.table.rows.push_back(row);
                }
                block.text = b.isMember("markdown") ? b["markdown"].asString() : block.table.toMarkdown();
            }
            else if (block.type == JZDocBlockType::Image)
            {
                block.image = b.isMember("image") ? b["image"].asInt() : -1;
                block.text = b.isMember("name") ? b["name"].asString() : std::string();
            }
            else
            {
                block.text = b.isMember("text") ? b["text"].asString() : std::string();
            }
            sec.blocks.push_back(block);
        }
        doc.sections.push_back(sec);
    }

    const Json::Value &imagesJson = root["images"];
    for (Json::ArrayIndex i = 0; i < imagesJson.size(); i++)
    {
        JZDocImage image;
        image.name = imagesJson[i].isMember("name") ? imagesJson[i]["name"].asString() : std::string();
        image.ext = imagesJson[i].isMember("ext") ? imagesJson[i]["ext"].asString() : std::string();

        if (imagesJson[i].isMember("data"))
        {
            std::string base64 = imagesJson[i]["data"].asString();
            image.data = JZByteArray::fromBase64(JZByteArray(base64.data(), (int)base64.size()));
        }
        else if (imagesJson[i].isMember("path"))
        {
            std::string path = imagesJson[i]["path"].asString();
            JZFile file;
            if (file.open(path, "rb"))
            {
                image.data = file.readAll();
                file.close();
            }
        }
        doc.images.push_back(image);
    }
    return JZResult<JZDocResult>::Ok(doc);
}

// ---------------- JZDocument ----------------
JZDocument::Format JZDocument::formatOfFile(const std::string &path)
{
    JZFileInfo info(path);
    std::string ext = str_toLower(info.suffix());
    if (ext == "docx")
        return FormatDocx;
    if (ext == "xlsx")
        return FormatXlsx;
    if (ext == "pdf")
        return FormatPdf;
    return FormatUnknown;
}

std::vector<std::string> JZDocument::supportedFormats()
{
    std::vector<std::string> formats;
    formats.push_back("docx");
    formats.push_back("xlsx");
    return formats;
}

JZResult<JZDocResult> JZDocument::open(const std::string &path)
{
    return open(path, formatOfFile(path));
}

JZResult<JZDocResult> JZDocument::open(const std::string &path, Format format)
{
    if (!JZFileInfo::exists(path))
        return JZResult<JZDocResult>::Error("document not found: " + path);

    switch (format)
    {
    case FormatDocx:
        return jzReadDocx(path);
    case FormatXlsx:
        return jzReadXlsx(path);
    case FormatPdf:
        // PDF 不把解析库链接进框架: 走 jzpdf(PDFium 内核) 独立进程 + stdout JSON。
        return jzReadPdf(path);
    default:
        return JZResult<JZDocResult>::Error("unsupported document format: " + path);
    }
}
