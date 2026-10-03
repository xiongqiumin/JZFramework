#pragma once
#include <memory>
#include <string>
#include "JZJSEngine.h"
#include "json/json.h"

// 把 OpenCV 的传统图像处理能力暴露给 JS: __jz.cv.*
//
// 定位: JZComputerUse 的 matchImage/matchColor 是"找一个东西"的定式接口; 本模块是
// **做图像本身**的通用工具(滤波/颜色转换/形态学/阈值/轮廓/绘制...), 给脚本自己做像素级处理。
// 典型用途: 容器里把虚拟桌面截图(JZImage / computeruse.screenshot)拿过来 → 去噪 → 颜色分割 →
// 形态学去毛刺 → findContours 找目标几何中心 → 点它。
//
// 设计(与 JZJSComputerUseModule 同一套约定, 见那个文件):
//   · **句柄传递**: C++ 侧一张表 map<int, cv::Mat>, JS 只拿一个 number(id)。
//     大图不走 base64/数组来回搬; 用完 release, 或引擎析构时统一清。
//   · **原子 API + 表驱动调度**: 句柄/编解码/像素访问是独立函数; 绝大多数运算走
//     op(name, paramsJson) 一个口 —— 加一项能力 = 表里加几行(见 .cpp 的 ops 表)。
//   · **失败必须显式**: op 失败返回 {ok:false, error:"..."}(JS 适配层转为抛错);
//     原子 API 用 0 / 空串 / {error} 表示失败。不用 -1 之类的魔法值。
//   · **缺 OpenCV 的构建**(JZ_CU_OPENCV 未定义, 例如没接 OpenCV 的 exe): 一律报
//     "未编入", 绝不假装成功(与 JZComputerUse 的退化纪律一致)。
//
// 接入方式(构建时补, 见 binary/code_agent/runtime 与 binary/virtual_device 的 CMakeLists):
//   m_cvModule = std::make_unique<JZJSCvModule>(); m_cvModule->initEngine(engine);
class JZImage;                                // JZWidget 的 RGBA8888 图像(见 jsFromImage; 只此一处用它)
class JZFRAMEWORK_API JZJSCvModule : public JZObject
{
public:
    JZJSCvModule();                       // 定义在 .cpp: PIMPL(unique_ptr<Impl>) 需要完整类型
    ~JZJSCvModule();

    void initEngine(JZJSEngine* engine);
    void deinitEngine();

private:
    struct Impl;                          // 句柄表; 只在 .cpp 里碰 cv::Mat(头文件不传染 OpenCV)
    std::unique_ptr<Impl> m_impl;
    JZJSEngine* m_engine = nullptr;

    // —— 桥接方法(参数/返回值由 jsbind 按 C++ 签名自动转换; Json::Value 自动转 JS 对象/数组) ——

    // 能力探测: 本构建是否编入了 OpenCV(没编时 JS 侧据此给出明确提示)
    bool        jsAvailable();

    // 句柄 / 生命周期
    int         jsNew(const std::string& optsJson);     // {cols,rows,type,kind?,fill?} -> id
    Json::Value jsInfo(int id);                         // {cols,rows,channels,depth,type,typeName,empty,total}
    int         jsClone(int id);                        // 深拷贝 -> 新 id(0 = 失败)
    bool        jsRelease(int id);
    int         jsReleaseAll();                         // 释放全部 -> 释放了几个
    int         jsCount();                              // 现在挂着几个句柄

    // 磁盘 / 编解码
    int         jsImread(const std::string& path);      // 磁盘 -> id(0 = 失败)
    bool        jsImwrite(int id, const std::string& path);
    int         jsImdecode(const std::string& data);    // base64 / dataURL -> id
    std::string jsImencode(int id, const std::string& ext);   // id -> dataURL(PNG/JPEG/...)
    std::string jsData(int id);                         // 原始像素字节(8U) -> base64
    int         jsFromData(const std::string& dataB64, const std::string& optsJson);  // {cols,rows,type}

    // 外部图像(框架自己的 JZImage; 例如 computeruse.screenshotImage() 给的那一帧)
    //   JZImage(RGBA8888, 行距紧凑) -> **拷一份**成 cv::Mat(8UC4) -> id(0 = 空图/失败)
    // ⚠ 两个"故意的"决定:
    //   ① **拷一份**, 不做零拷贝视图 —— 拷完就与那个 JZImage 无关, 没有"原图先释放 ⇒ Mat 悬垂"
    //      的问题(JZComputerUse.cpp 里那个零拷贝的 jzCvMatOf 只在一次调用内部活着)。
    //      一帧几 MB 的 memcpy 是亚毫秒级, 比"编码成 PNG 再解码回来"便宜两个数量级。
    //   ② **只搬不转**: 通道序照原样(R,G,B,A)。要 BGR/灰度请接着 op('cvtColor', {code:'RGBA2BGR'})
    //      —— "搬"和"转"分开, 一眼能看出手上这张图是什么通道(见 .channels())。
    int         jsFromImage(const JZImage& img);

    // 逐像素(脚本里做少量采样/验证用; 大批量请用 op + 掩膜)
    Json::Value jsPixel(int id, int x, int y);          // [v0,v1,...] / null
    bool        jsSetPixel(int id, int x, int y, const std::string& valuesJson);

    // 通用运算: 表驱动(名字见 jsOps()); 返回 {ok:true, mat|mats|value} 或 {ok:false,error}
    Json::Value jsOp(const std::string& name, const std::string& paramsJson);
    Json::Value jsOps();                                // 支持的操作名清单(自检/文档用)
};
