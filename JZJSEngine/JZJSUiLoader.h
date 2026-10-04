#ifndef JZ_JS_UI_LOADER_H_
#define JZ_JS_UI_LOADER_H_

#include "JZGlobal.h"
#include <string>

#include "quickjs/quickjs.h"
#include "JZUiParser.h"   // 解析 + 方言(公用; C++ 侧 JZWidget/JZUiLoader 也用这份)

class JZJSEngine;

// ---------------------------------------------------------------------------
// JZJSUiLoader —— **JS 侧**的 UI 加载器: 解析(继承 JZUiParser) + 生成。
//
// 生成这一段是 JS 后端独有的:
//   * generateTsClass / toTsClass: 生成 TS 类源码(Qt uic 风格);
//   * createUi: 把树生成可执行 JS 脚本, 经 engine->eval 构造真实控件树(返回 { root, <id>: widget })。
// C++ 后端在 JZWidget/JZUiLoader(直接 new 真 JZWidget), 两边共用 JZUiParser 的解析与方言表。
//
// 注意: 本文件只在启用 JZWidget 的构建里参与编译(见 JZJSEngine.cmake 的 if(HAS_JZWidget)) ——
// UI 加载器离不开控件族。
// ---------------------------------------------------------------------------
class JZFRAMEWORK_API JZJSUiLoader : public JZUiParser
{
public:
    JZJSUiLoader();

    // 生成与 @jz/widget 控件构建器对齐的 TypeScript 类源码(Qt uic 风格)。
    // className 覆盖 XML 中的 <class>; importPath 为 'widget' 模块导入路径(默认 "widget")。
    // 未加载(load 失败)时返回 false, 原因见 error()。
    // 注意: 重复 id 会产生同名字段(非法 TS), 调用方需保证 id 唯一。
    bool generateTsClass(std::string& outTs,
                         const std::string& className = "",
                         const std::string& importPath = "widget") const;

    // generateTsClass 的便捷版: 成功返回生成源码, 失败返回空串(原因见 error())。
    std::string toTsClass(const std::string& className = "",
                          const std::string& importPath = "widget") const;

    // 基于已加载的 UI 定义, 在给定引擎中构造真实控件树并直接返回构造好的 JSValue。
    // 等价于 generateTsClass 的运行时版: 生成可执行的 JS(直接 new 各 JZWidget 子类、设置属性、
    // 挂载父子关系), 经 engine->eval 执行, 返回形如 { root, <id>: widget } 的 JS 对象。
    // 失败(未 load / 含未知控件类型 / eval 抛异常)返回 JS_UNDEFINED, 原因见 error()。
    // 注意: 调用前引擎须已由 JZJSWidgetModule::initEngine 注册全部控件类(new JZWindow 之类可用)。
    JSValue createUi(JZJSEngine* engine) const;
};

#endif // !JZ_JS_UI_LOADER_H_
