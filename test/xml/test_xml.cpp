#include "JZTest.h"
#include "JZXml.h"
#include "JZApplication.h"
#include "JZDebug.h"

#include <string>

static const char* kSampleXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<!-- 书籍列表 -->
<!DOCTYPE catalog PUBLIC "-//Example//DTD//EN" "http://example.com/catalog.dtd">
<catalog xmlns:bk="http://example.com/book">
  <book id="1" category="fiction">
    <title>XML &amp; DOM 入门</title>
    <author>张三</author>
    <price>39.5</price>
    <desc><![CDATA[<special> 内容 </special>]]></desc>
  </book>
  <book id="2" category="tech">
    <title>Cpp &lt;T&gt; 编程</title>
    <author>李四</author>
    <price>59</price>
  </book>
  <?process-instruction sample data?>
</catalog>
)";

bool jzxml_parse_basic()
{
    JZDomDocument doc;
    std::string err;
    JZ_TEST_TRUE(doc.setContent(kSampleXml, &err));
    JZ_TEST_TRUE(doc.documentElement().isElement());
    JZ_TEST_EQ(doc.documentElement().tagName(), "catalog");

    JZDomDocumentType dt = doc.doctype();
    JZ_TEST_TRUE(dt.isDocumentType());
    JZ_TEST_EQ(dt.name(), "catalog");
    JZ_TEST_EQ(dt.publicId(), "-//Example//DTD//EN");
    JZ_TEST_EQ(dt.systemId(), "http://example.com/catalog.dtd");
    return true;
}

bool jzxml_navigation_and_attrs()
{
    JZDomDocument doc;
    JZ_TEST_TRUE(doc.setContent(kSampleXml));

    JZDomNodeList books = doc.elementsByTagName("book");
    JZ_TEST_EQ(books.count(), 2);

    JZDomElement first = books.item(0).toElement();
    JZ_TEST_EQ(first.attribute("id"), "1");
    JZ_TEST_EQ(first.attribute("category"), "fiction");
    JZ_TEST_TRUE(first.hasAttribute("id"));
    JZ_TEST_FALSE(first.hasAttribute("nope"));

    // 文本拼接 (title)
    JZDomElement title = first.elementsByTagName("title").item(0).toElement();
    JZ_TEST_EQ(title.text(), "XML & DOM 入门");

    // 嵌套 price
    JZDomElement price = first.elementsByTagName("price").item(0).toElement();
    JZ_TEST_EQ(price.text(), "39.5");
    return true;
}

bool jzxml_cdata_and_comment_and_pi()
{
    JZDomDocument doc;
    JZ_TEST_TRUE(doc.setContent(kSampleXml));

    JZDomElement first = doc.elementsByTagName("book").item(0).toElement();
    JZDomElement desc = first.elementsByTagName("desc").item(0).toElement();
    JZDomNodeList children = desc.childNodes();
    bool hasCdata = false;
    for (int i = 0; i < children.count(); i++)
    {
        if (children.item(i).isCDATASection())
        {
            hasCdata = true;
            JZ_TEST_EQ(children.item(i).nodeValue(), "<special> 内容 </special>");
        }
    }
    JZ_TEST_TRUE(hasCdata);

    // 文档级注释
    JZDomNodeList docChildren = doc.childNodes();
    bool hasComment = false;
    for (int i = 0; i < docChildren.count(); i++)
        if (docChildren.item(i).isComment()) hasComment = true;
    JZ_TEST_TRUE(hasComment);

    // 处理指令
    bool hasPI = false;
    for (int i = 0; i < docChildren.count(); i++)
    {
        if (docChildren.item(i).isProcessingInstruction())
        {
            hasPI = true;
            JZ_TEST_EQ(docChildren.item(i).toProcessingInstruction().target(), "process-instruction");
            JZ_TEST_EQ(docChildren.item(i).toProcessingInstruction().data(), "sample data");
        }
    }
    JZ_TEST_TRUE(hasPI);
    return true;
}

bool jzxml_entities()
{
    JZDomDocument doc;
    const char* xml = "<r a=\"&lt;&quot;x&quot;&gt;\">v &amp; &lt; &gt; &#65; &#x42;</r>";
    JZ_TEST_TRUE(doc.setContent(xml));
    JZDomElement r = doc.documentElement();
    JZ_TEST_EQ(r.attribute("a"), "<\"x\">");
    JZ_TEST_EQ(r.text(), "v & < > A B");
    return true;
}

bool jzxml_error_reporting()
{
    JZDomDocument doc;
    std::string err;
    int line = 0, col = 0;
    const char* bad = "<root><a></b></root>";
    JZ_TEST_FALSE(doc.setContent(bad, &err, &line, &col));
    JZ_TEST_FALSE(err.empty());
    jzInfo() << "jzxml error:" << err << "line" << line << "col" << col;

    // 未闭合
    JZDomDocument doc2;
    JZ_TEST_FALSE(doc2.setContent("<root><a>", nullptr));
    return true;
}

bool jzxml_build_and_serialize()
{
    JZDomDocument doc;
    JZDomElement root = doc.createElement("root");
    doc.appendChild(root);

    JZDomElement item = doc.createElement("item");
    item.setAttribute("id", 7);
    item.setAttribute("name", "测试");
    item.setText("hello & <world>");
    root.appendChild(item);

    std::string out = doc.toString(2);
    jzInfo() << "jzxml built:\n" << out;

    // 回环: 重新解析
    JZDomDocument doc2;
    JZ_TEST_TRUE(doc2.setContent(out));
    JZDomElement root2 = doc2.documentElement();
    JZ_TEST_EQ(root2.tagName(), "root");
    JZDomElement item2 = root2.elementsByTagName("item").item(0).toElement();
    JZ_TEST_EQ(item2.attribute("id"), "7");
    JZ_TEST_EQ(item2.attribute("name"), "测试");
    JZ_TEST_EQ(item2.text(), "hello & <world>");
    return true;
}

bool jzxml_clone_and_edit()
{
    JZDomDocument doc;
    JZ_TEST_TRUE(doc.setContent(kSampleXml));
    JZDomElement book = doc.elementsByTagName("book").item(0).toElement();

    JZDomNode cloned = book.cloneNode(true);
    JZ_TEST_TRUE(cloned.isElement());
    JZ_TEST_EQ(cloned.toElement().attribute("id"), "1");
    JZ_TEST_EQ(cloned.toElement().elementsByTagName("title").item(0).toElement().text(), "XML & DOM 入门");

    // appendChild 重父: 克隆节点加入新文档后, 其 ownerDocument 应为新文档
    JZDomDocument doc2;
    JZDomElement r2 = doc2.createElement("wrap");
    doc2.appendChild(r2);
    r2.appendChild(cloned);
    JZ_TEST_EQ(cloned.ownerDocument().documentElement().tagName(), "wrap");

    // removeChild
    JZDomNodeList kids = book.childNodes();
    int before = kids.count();
    book.removeChild(book.elementsByTagName("author").item(0));
    JZ_TEST_EQ(book.childNodes().count(), before - 1);
    return true;
}

int main(int argc, char* argv[])
{
    JZApplication app;

    JZTest test;
    auto suite = std::make_shared<JZTestSuite>("xml");
    suite->addTestCase("parse_basic", jzxml_parse_basic);
    suite->addTestCase("navigation_and_attrs", jzxml_navigation_and_attrs);
    suite->addTestCase("cdata_comment_pi", jzxml_cdata_and_comment_and_pi);
    suite->addTestCase("entities", jzxml_entities);
    suite->addTestCase("error_reporting", jzxml_error_reporting);
    suite->addTestCase("build_and_serialize", jzxml_build_and_serialize);
    suite->addTestCase("clone_and_edit", jzxml_clone_and_edit);
    test.addSuite(suite);

    return test.run(argc, argv);
}
