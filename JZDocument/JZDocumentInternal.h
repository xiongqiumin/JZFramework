#ifndef JZ_DOCUMENT_INTERNAL_H_
#define JZ_DOCUMENT_INTERNAL_H_

#include <map>
#include <string>
#include <vector>

#include "JZCompress/JZCompress.h"
#include "JZXml/JZXml.h"

// ============================================================================
// JZDocument 内部工具: docx / xlsx 解析共用, 不对外暴露(不安装到公共接口)
// ============================================================================
namespace JZDocInternal
{
// 从 zip 读出条目并解析为 DOM。失败返回 false, 错误写入 err。
bool readXml(JZUnZip &zip, const std::string &entry, JZDomDocument &doc, std::string &err);

// 去掉命名空间前缀: "w:p" → "p"(有些工具生成无前缀的 OOXML, 匹配时统一按本地名比)
std::string localName(const std::string &tag);

// 取属性(同样忽略前缀 + 忽略大小写: "w:val" / "val" / "Val" 都能取到)
std::string attrValue(const JZDomElement &e, const std::string &name);

// 直接子元素(按 tag 过滤, 保持文档顺序)
std::vector<JZDomElement> childElements(const JZDomNode &node, const std::string &tag);
JZDomElement childElement(const JZDomNode &node, const std::string &tag);

// 递归收集所有后代 <t> 的文本(跳过日文注音 <rPh>): xlsx 的 <si> / <is>
void collectTextNodes(const JZDomNode &node, std::string &out);

// docx 段落文本: <w:t> 文本 + <w:tab>→\t + <w:br>/<w:cr>→\n, 跳过域指令 <w:instrText> / 修订删除 <w:delText>
void collectParagraphText(const JZDomNode &node, std::string &out);

// 递归查找(深层元素, 如 <w:drawing> 里的 <a:blip>)
JZDomElement findElement(const JZDomNode &node, const std::string &tag);
void findElements(const JZDomNode &node, const std::string &tag, std::vector<JZDomElement> &out);

// 单元格引用 "B12" → 列号(0 基); 非法返回 -1
int columnFromRef(const std::string &ref);

// zip 内路径解析: baseDir="word/", target="media/a.png" → "word/media/a.png"
// 支持 target 以 "/" 开头(包内绝对路径)与 "../"、"./"
std::string resolveTarget(const std::string &baseDir, const std::string &target);

// 数字 → 字符串(整数不带小数点, 避免 18 → "18.000000")
std::string formatNumber(double v);

// numFmt 是否为日期/时间格式(builtin id + 自定义 formatCode 判定)
bool isDateNumFmt(int numFmtId, const std::string &formatCode);

// Excel 日期序列号 → "YYYY-MM-DD"(小数部分非 0 时追加 " HH:MM:SS"); serial < 1 视为纯时间 "HH:MM:SS"
std::string serialToDate(double serial);

// markdown 单元格转义: | → \|, 换行 → <br>, 制表符 → 空格
std::string markdownCell(const std::string &text);

std::string trimmed(const std::string &text);
std::string trimmedRight(const std::string &text);
std::string fileExt(const std::string &name);   // "a/b.PNG" → "png"
}

#endif // !JZ_DOCUMENT_INTERNAL_H_
