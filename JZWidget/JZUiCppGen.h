#ifndef JZ_UI_CPP_GEN_H_
#define JZ_UI_CPP_GEN_H_

#include <set>
#include <string>
#include <vector>
#include "JZUiParser.h"

// ---------------------------------------------------------------------------
// JZUiCppGen —— **.ui(uiLoader 方言) -> C++ 代码**生成器(uic 风格)。
//
// 为什么要它: .ui 文件继续当源头(不动), 但 C++ 宿主希望像 Qt 那样
//     #include "ui_BTEServer.h"
//     Ui::BTEServerUi ui;  ui.setupUi();  ui.listWidget->...
// 即"构建期生成代码"而不是运行期解析 XML —— 启动不再解析、成员有类型、改 .ui 会重新编译。
// 构建期入口见 CMake 函数 jz_add_ui()(JZFramework.cmake) + 工具 tools/jz_uic。
//
// 同源: 解析/方言判别**全部**复用 JZUiParser(类映射 classForType / 挂载 mountMode /
//       tabLabel / 属性白名单 hasText... / layoutMarginIndex), 因此三个后端对同一份 .ui 的
//       理解一致:
//         * JZUiLoader      : 运行期按 XML 直接建控件;
//         * JZJSUiLoader    : 生成 JS 脚本 -> eval;
//         * JZUiCppGen(本类): 生成 C++ 源码 -> 编译进宿主。
//
// 生成物形状(头文件自足):
//     namespace Ui {
//     class BTEServerUi {
//     public:
//         JZWidget*     root = nullptr;      // 顶层控件
//         JZMainWindow* main = nullptr;      // 带 name 的控件各一个成员(类型 = 引擎类)
//         JZListWidget* listWidget = nullptr;
//         std::map<std::string, JZWidget*> byId;   // 与 JZUiLoader::Result::byId 同义
//         void setupUi();
//     };
//     }
//
// 覆盖的属性与 JZUiLoader::applyProps 同一份口径(text/title/currentIndex/checked/readOnly/
// placeholderText/minimum/maximum/value/suffix/specialValueText/singleStep/decimals/prefix/
// dateMsecs/displayFormat/calendarPopup/visible/geometry/styleSheet/style), 其余丢弃。
// 已知限制(生成时记进 warnings() 或直接报错, 不会静默出错):
//   * 布局嵌套布局: 引擎本就不支持, 这里与 loader 一样报错(error() 非空);
//   * 未走 JZUiParser 白名单的属性(如 styleClass/shortcut)不生成。
// ---------------------------------------------------------------------------
class JZFRAMEWORK_API JZUiCppGen
{
public:
    // root: 已解析的 .ui 根节点(见 JZUiParser::root()); uiClass: 生成的 C++ 类名(如 "BTEServerUi")
    JZUiCppGen(const JZUiNode& root, const std::string& uiClass);

    // 生成完整头文件内容; 出错返回空串(原因见 error())
    std::string generate();

    const std::string& error() const { return m_error; }
    const std::vector<std::string>& warnings() const { return m_warnings; }

private:
    std::string genWidget(const JZUiNode& node);          // 生成一个控件节点, 返回其局部变量名
    std::string genLayout(const JZUiNode& node, const std::string& hostVar);   // 生成布局 + 把子控件加进去
    void        genChildren(const JZUiNode& node, const std::string& selfVar);
    void        genMenuBar(const JZUiNode& menuBarNode, const std::string& windowVar);
    void        genProps(const JZUiNode& node, const std::string& var);
    void        genLayoutProps(const JZUiNode& node, const std::string& layVar);

    void        collectMembers(const JZUiNode& node);     // 预扫: 成员表 + 需要的头文件 + 结构校验
    // 预扫: 同名控件改名 + 告警(见 .cpp 里的说明)。生成前对内部副本做, 不影响调用方的树。
    void        dedupNames(JZUiNode& node, std::set<std::string>& used);

    void        line(const std::string& s);               // 写一行(带缩进)
    std::string newVar();
    std::string memberName(const std::string& id) const;  // id -> 合法 C++ 标识符(byId 里仍是原 id)

    void        setError(const std::string& msg) { if (m_error.empty()) m_error = msg; }

private:
    // 存值(不是引用): generate() 会先对树做一遍"重名改名", 副本才敢改。
    JZUiNode        m_root;
    std::string     m_uiClass;
    std::string     m_error;
    std::vector<std::string> m_warnings;

    std::string  m_body;                  // setupUi 函数体
    std::string  m_members;               // public 成员声明
    std::vector<std::string> m_headers;   // 用到的头(去重后排序)
    int          m_varCounter = 0;
};

#endif // !JZ_UI_CPP_GEN_H_
