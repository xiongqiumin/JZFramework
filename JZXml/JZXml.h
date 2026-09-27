#ifndef JZ_XML_H_
#define JZ_XML_H_
#include "JZGlobal.h"
#include <string>
#include <vector>
#include <memory>

// ============================================================================
// JZXml —— Qt 风格 (QDom*) 的 XML DOM 解析模块
//
// 设计要点:
//  - 公开类 (JZDomNode / JZDomElement / ...) 都是「轻量句柄」, 内部统一持有
//    std::shared_ptr<JZDomNodeData>。整棵树由 JZDomDocument 拥有, 因此只要
//    JZDomDocument 句柄存活, 其下的所有节点句柄都有效 (与 Qt 的隐式共享语义一致)。
//  - 仅依赖标准库 (C++14), 公开接口使用 UTF-8 的 std::string。
//  - 解析支持: XML 声明、注释、处理指令、CDATA、DOCTYPE、实体
//    (&amp; &lt; &gt; &quot; &apos; 以及 &#nn; / &#xHH; 数字实体)。
// ============================================================================

// 内部节点数据。公开为简单结构体, 便于派生类直接访问。
class JZFRAMEWORK_API JZDomNodeData : public std::enable_shared_from_this<JZDomNodeData>
{
public:
    enum class Type
    {
        Document,
        Element,
        Attribute,
        Text,
        CDATASection,
        Comment,
        ProcessingInstruction,
        DocumentType,
        EntityReference,
        Notation,
        DocumentFragment
    };

    Type type = Type::Document;
    std::string name;                 // tagName / attr name / PI target / doctype name ...
    std::string value;                // nodeValue / attr value / text data / PI data
    std::string publicId;             // 仅 DocumentType / Notation
    std::string systemId;             // 仅 DocumentType / Notation
    JZDomNodeData* parent = nullptr;  // 反向指针 (不持有所有权)
    std::vector<std::shared_ptr<JZDomNodeData>> children;   // 子节点
    std::vector<std::shared_ptr<JZDomNodeData>> attributes; // 仅 Element 使用
};

using JZDomNodeDataPtr = std::shared_ptr<JZDomNodeData>;

// 前置声明
class JZDomNode;
class JZDomNodeList;
class JZDomNamedNodeMap;
class JZDomElement;
class JZDomAttr;
class JZDomCharacterData;
class JZDomText;
class JZDomCDATASection;
class JZDomComment;
class JZDomDocument;
class JZDomProcessingInstruction;
class JZDomDocumentType;
class JZDomDocumentFragment;
class JZDomEntityReference;
class JZDomNotation;

// 节点类型 (取值对齐 Qt QDomNode::NodeType)
enum JZDomNodeType
{
    JZDomElementNode = 1,
    JZDomAttributeNode = 2,
    JZDomTextNode = 3,
    JZDomCDATASectionNode = 4,
    JZDomEntityReferenceNode = 5,
    JZDomEntityNode = 6,
    JZDomProcessingInstructionNode = 7,
    JZDomCommentNode = 8,
    JZDomDocumentNode = 9,
    JZDomDocumentTypeNode = 10,
    JZDomDocumentFragmentNode = 11,
    JZDomNotationNode = 12
};

// ============================================================================
// JZDomNodeList
// ============================================================================
class JZFRAMEWORK_API JZDomNodeList
{
public:
    JZDomNodeList();
    explicit JZDomNodeList(const std::vector<JZDomNodeDataPtr>& nodes);

    bool isEmpty() const;
    int count() const;
    int length() const;               // 兼容 Qt (等同 count)
    JZDomNode item(int index) const;
    JZDomNode operator[](int index) const;

private:
    std::vector<JZDomNodeDataPtr> m_nodes;
    friend class JZDomNode;
    friend class JZDomElement;
};

// ============================================================================
// JZDomNamedNodeMap (属性集合)
// ============================================================================
class JZFRAMEWORK_API JZDomNamedNodeMap
{
public:
    JZDomNamedNodeMap();
    explicit JZDomNamedNodeMap(const std::vector<JZDomNodeDataPtr>& attrs);

    int count() const;
    int length() const;
    JZDomNode item(int index) const;
    JZDomNode namedItem(const std::string& name) const;
    bool contains(const std::string& name) const;
    JZDomNode setNamedItem(const JZDomNode& node);
    JZDomNode removeNamedItem(const std::string& name);

private:
    std::vector<JZDomNodeDataPtr> m_nodes;
    friend class JZDomNode;
    friend class JZDomElement;
};

// ============================================================================
// JZDomNode —— DOM 树基类
// ============================================================================
class JZFRAMEWORK_API JZDomNode
{
public:
    JZDomNode();
    explicit JZDomNode(const JZDomNodeDataPtr& data);
    virtual ~JZDomNode();

    bool isNull() const;

    // ---- 基础属性 ----
    int nodeType() const;
    std::string nodeName() const;
    std::string nodeValue() const;
    void setNodeValue(const std::string& v);

    // ---- 树导航 ----
    JZDomNode parentNode() const;
    JZDomNode firstChild() const;
    JZDomNode lastChild() const;
    JZDomNode nextSibling() const;
    JZDomNode previousSibling() const;
    JZDomNodeList childNodes() const;
    JZDomNamedNodeMap attributes() const;

    bool hasChildNodes() const;
    bool hasAttributes() const;

    // ---- 类型判定 ----
    bool isElement() const;
    bool isAttr() const;
    bool isText() const;
    bool isCDATASection() const;
    bool isComment() const;
    bool isDocument() const;
    bool isDocumentFragment() const;
    bool isProcessingInstruction() const;
    bool isDocumentType() const;
    bool isEntityReference() const;
    bool isNotation() const;
    bool isCharacterData() const;

    // ---- 类型转换 ----
    JZDomElement toElement() const;
    JZDomAttr toAttr() const;
    JZDomText toText() const;
    JZDomCDATASection toCDATASection() const;
    JZDomComment toComment() const;
    JZDomDocument toDocument() const;
    JZDomProcessingInstruction toProcessingInstruction() const;
    JZDomDocumentType toDocumentType() const;
    JZDomEntityReference toEntityReference() const;
    JZDomNotation toNotation() const;
    JZDomDocumentFragment toDocumentFragment() const;

    // ---- 查找 ----
    JZDomNode namedItem(const std::string& name) const;           // 仅 Element 有效
    JZDomNodeList elementsByTagName(const std::string& tagname) const;

    // ---- 编辑 ----
    JZDomNode insertBefore(const JZDomNode& newChild, const JZDomNode& refChild);
    JZDomNode replaceChild(const JZDomNode& newChild, const JZDomNode& oldChild);
    JZDomNode removeChild(const JZDomNode& oldChild);
    JZDomNode appendChild(const JZDomNode& newChild);
    JZDomNode cloneNode(bool deep = true) const;

    JZDomDocument ownerDocument() const;

    // ---- 序列化 ----
    // indent>0: 每层缩进 indent 个空格; indent<=0: 紧凑 (无换行)
    std::string toString(int indent = 1) const;

protected:
    JZDomNodeDataPtr m_data;

    friend class JZDomDocument;
    friend class JZDomElement;
    friend class JZDomAttr;
    friend class JZDomNodeList;
    friend class JZDomNamedNodeMap;
};

// ============================================================================
// JZDomCharacterData —— Text / Comment / CDATASection 的基类
// ============================================================================
class JZFRAMEWORK_API JZDomCharacterData : public JZDomNode
{
public:
    JZDomCharacterData();
    explicit JZDomCharacterData(const JZDomNodeDataPtr& data);

    std::string data() const;
    void setData(const std::string& data);

    int length() const;
    std::string substringData(int offset, int count) const;
    void appendData(const std::string& arg);
    void insertData(int offset, const std::string& arg);
    void deleteData(int offset, int count);
    void replaceData(int offset, int count, const std::string& arg);
};

// ============================================================================
// JZDomElement
// ============================================================================
class JZFRAMEWORK_API JZDomElement : public JZDomNode
{
public:
    JZDomElement();
    explicit JZDomElement(const JZDomNodeDataPtr& data);

    std::string tagName() const;
    void setTagName(const std::string& name);

    std::string attribute(const std::string& name, const std::string& defValue = std::string()) const;
    void setAttribute(const std::string& name, const std::string& value);
    void setAttribute(const std::string& name, int value);
    void setAttribute(const std::string& name, double value);
    void removeAttribute(const std::string& name);
    bool hasAttribute(const std::string& name) const;

    JZDomAttr attributeNode(const std::string& name) const;
    JZDomAttr setAttributeNode(const JZDomAttr& newAttr);
    JZDomAttr removeAttributeNode(const JZDomAttr& oldAttr);

    // 连接所有后代文本节点
    std::string text() const;
    // 便捷: 清空子节点并写入单个文本节点
    void setText(const std::string& text);

    JZDomNodeList elementsByTagName(const std::string& tagname) const;
};

// ============================================================================
// JZDomAttr
// ============================================================================
class JZFRAMEWORK_API JZDomAttr : public JZDomNode
{
public:
    JZDomAttr();
    explicit JZDomAttr(const JZDomNodeDataPtr& data);

    std::string name() const;
    std::string value() const;
    void setValue(const std::string& v);
    bool specified() const;
    JZDomElement ownerElement() const;
};

// ============================================================================
// JZDomText
// ============================================================================
class JZFRAMEWORK_API JZDomText : public JZDomCharacterData
{
public:
    JZDomText();
    explicit JZDomText(const JZDomNodeDataPtr& data);

    // 在 offset 处拆分, 返回 [offset, end) 部分作为新 Text 节点
    JZDomText splitText(int offset);
};

// ============================================================================
// JZDomCDATASection
// ============================================================================
class JZFRAMEWORK_API JZDomCDATASection : public JZDomCharacterData
{
public:
    JZDomCDATASection();
    explicit JZDomCDATASection(const JZDomNodeDataPtr& data);
};

// ============================================================================
// JZDomComment
// ============================================================================
class JZFRAMEWORK_API JZDomComment : public JZDomCharacterData
{
public:
    JZDomComment();
    explicit JZDomComment(const JZDomNodeDataPtr& data);
};

// ============================================================================
// JZDomProcessingInstruction
// ============================================================================
class JZFRAMEWORK_API JZDomProcessingInstruction : public JZDomNode
{
public:
    JZDomProcessingInstruction();
    explicit JZDomProcessingInstruction(const JZDomNodeDataPtr& data);

    std::string target() const;
    std::string data() const;
    void setData(const std::string& d);
};

// ============================================================================
// JZDomDocumentType
// ============================================================================
class JZFRAMEWORK_API JZDomDocumentType : public JZDomNode
{
public:
    JZDomDocumentType();
    explicit JZDomDocumentType(const JZDomNodeDataPtr& data);

    std::string name() const;
    std::string publicId() const;
    std::string systemId() const;
};

// ============================================================================
// JZDomEntityReference
// ============================================================================
class JZFRAMEWORK_API JZDomEntityReference : public JZDomNode
{
public:
    JZDomEntityReference();
    explicit JZDomEntityReference(const JZDomNodeDataPtr& data);

    std::string name() const;
};

// ============================================================================
// JZDomNotation
// ============================================================================
class JZFRAMEWORK_API JZDomNotation : public JZDomNode
{
public:
    JZDomNotation();
    explicit JZDomNotation(const JZDomNodeDataPtr& data);

    std::string name() const;
    std::string publicId() const;
    std::string systemId() const;
};

// ============================================================================
// JZDomDocumentFragment
// ============================================================================
class JZFRAMEWORK_API JZDomDocumentFragment : public JZDomNode
{
public:
    JZDomDocumentFragment();
    explicit JZDomDocumentFragment(const JZDomNodeDataPtr& data);
};

// ============================================================================
// JZDomDocument
// ============================================================================
class JZFRAMEWORK_API JZDomDocument : public JZDomNode
{
public:
    JZDomDocument();
    explicit JZDomDocument(const JZDomNodeDataPtr& data);

    void clear();

    // 解析 XML 文本。失败返回 false 并通过 out 参数给出错误信息。
    bool setContent(const std::string& text,
                    std::string* errorMsg = nullptr,
                    int* errorLine = nullptr,
                    int* errorColumn = nullptr);
    bool setContent(const char* text,
                    std::string* errorMsg = nullptr,
                    int* errorLine = nullptr,
                    int* errorColumn = nullptr);

    // 是否存在根元素
    bool isEmpty() const;

    JZDomElement documentElement() const;
    JZDomDocumentType doctype() const;

    // 工厂方法
    JZDomElement createElement(const std::string& tagName);
    JZDomText createTextNode(const std::string& data);
    JZDomComment createComment(const std::string& data);
    JZDomCDATASection createCDATASection(const std::string& data);
    JZDomAttr createAttribute(const std::string& name);
    JZDomProcessingInstruction createProcessingInstruction(const std::string& target, const std::string& data);
    JZDomDocumentFragment createDocumentFragment();
    JZDomEntityReference createEntityReference(const std::string& name);

    // 从其它文档导入节点 (deep=true 递归)
    JZDomNode importNode(const JZDomNode& importedNode, bool deep = true);

    JZDomNodeList elementsByTagName(const std::string& tagname) const;

    // 序列化 (不自动加 XML 声明, 与 Qt 一致)
    std::string toString(int indent = 1) const;
};

#endif // !JZ_XML_H_
