#ifndef JZ_SOURCE_MAP_H_
#define JZ_SOURCE_MAP_H_

#include <functional>
#include <map>
#include <string>
#include <vector>

// ------------------------------------------------------------------
// source map v3 翻译器：把 JS 产物里的位置反查回 TS 源码位置
//
//   tsc 打开 "sourceMap": true 后，产物旁边会有一份同名 .js.map。本类按
//   source map v3 规范做"生成位置 -> 原始位置"反查，并负责：
//     - 解析 .map（JSON + Base64 VLQ 的 mappings），带缓存（失效的也缓存，
//       避免反复读盘）；
//     - 单个 "<js 文件>:行:列" 帧的翻译（mapLocation）；
//     - 整段文本（一条错误栈）里所有帧的批量翻译（mapStack）。
//
//   运行根 root() 的两个作用：
//     1) 栈里的相对模块名（"build/lib"）按它解析；
//     2) 翻译出的源路径尽量显示成它的相对路径（"src/x.ts"），
//        指向根之外时保留原样，不谎报位置。
//
//   典型用法：
//         JZSourceMap sm;
//         sm.setRoot(projectDir);
//         printf("%s", sm.mapStack(jsErrorText).c_str());
//
//   注：这里只做位置翻译，不认识 TS 语法、不解释栈语义、不做编译；
//   路径操作一律走框架 JZDir / JZFile / JZFileInfo。
// ------------------------------------------------------------------
class JZSourceMap
{
public:
    // 翻译后的源码位置
    struct Location
    {
        std::string file;   // 源文件的规范绝对路径（相对化留给调用方，见 mapStack 的 mapPath）
        int line = 0;       // 1-based
        int column = 0;     // 1-based
        std::string name;   // map 里带的原始标识符（可能为空）
    };

    JZSourceMap();

    // 运行根（js 产物与 .map 所在的目录树的根）。只用于"解析栈里的相对模块名"
    // （如 "build/lib"），不做任何显示形态的转换——解析结果一律是规范化的
    // 绝对路径，与官方 source-map 库的 _absoluteSources 一致。为空时 js 路径按原样解析。
    void setRoot(const std::string &root);
    const std::string &root() const;

    // 映射成功时是否附带原始 js 位置
    //   false(默认): "src/sort.ts:12:7"
    //   true       : "src/sort.ts:12:7 (js: build/sort.js:8:5)"
    // 映射失败时无论该开关如何，都保持原文不动。
    void setKeepOriginal(bool keep);
    bool keepOriginal() const;

    // 清空已加载的 map 缓存（产物重新编译后调用，否则会读到旧映射）
    void clearCache();

    // 整段文本：把其中所有 "<xxx.js>:行:列" 的帧逐个尝试翻译；
    // 找不到 map 或该位置无映射的帧保持原样，不影响其它帧。
    //
    // mapPath 可选：把解析出的"源文件绝对路径"转成想显示的形态（如相对工程根的
    // "src/x.ts"）。官方 source-map 库不做这件事（它只给规范化的 source URL），
    // "相对谁显示"是调用方的事；不传就原样输出绝对路径。
    typedef std::function<std::string(const std::string &absSource)> PathMapper;
    std::string mapStack(const std::string &text, const PathMapper &mapPath = PathMapper()) const;

    // 单帧翻译：jsFile 为 js 产物路径（绝对，或相对运行根）；
    // line / column 与 V8 / QuickJS 栈一致，均为 1-based。
    // 成功时 out.file 是规范化的源文件绝对路径（不做相对化）。
    bool mapLocation(const std::string &jsFile, int line, int column, Location &out) const;

private:
    // source map 中一个"段"（同一生成行内按生成列递增）
    struct Segment
    {
        int genColumn = 0;      // 生成文件列（0-based）
        int srcIndex = 0;       // sources 下标
        int srcLine = 0;        // 源文件行（0-based）
        int srcColumn = 0;      // 源文件列（0-based）
        int nameIndex = -1;     // names 下标，-1 表示无
    };

    struct SourceMap
    {
        bool valid = false;     // 解析成功（false 也会被缓存，避免反复读盘）
        std::string mapDir;     // map 所在目录，用于解析 sources / sourceRoot
        std::string sourceRoot;
        std::vector<std::string> sources;
        std::vector<std::string> names;
        std::vector<std::vector<Segment> > lines;   // 下标 = 生成文件行号(0-based)
    };

    // 取 js 产物对应的 map（带缓存）；没有可用 map 返回 nullptr
    const SourceMap *findMap(const std::string &jsFile) const;

    static bool parseMapFile(const std::string &mapPath, SourceMap &out);
    static bool parseMappings(const std::string &mappings, SourceMap &out);
    static bool decodeVLQ(const std::string &s, size_t &pos, int &value);

    std::string resolveSource(const SourceMap &map, int srcIndex) const;

    std::string m_root;
    bool m_keepOriginal = false;
    mutable std::map<std::string, SourceMap> m_maps;   // 规范化后的 js 路径 -> map
};

#endif // !JZ_SOURCE_MAP_H_
