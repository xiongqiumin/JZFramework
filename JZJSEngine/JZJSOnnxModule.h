#pragma once
#include <memory>
#include <string>
#include "JZJSEngine.h"
#include "json/json.h"

// 把 ONNX Runtime 的推理能力暴露给 JS: __jz.onnx.*
//
// 定位: 跑**任意** .onnx 模型(分类/检测/分割/embedding...), 不关心模型是干嘛的 ——
//   computeruse 的 matchImage/matchColor 是"定式查找", JZJSCvModule 是"处理像素",
//   本模块是"喂张量、拿张量"。三者都在给脚本做"看得见 + 算得出"的工具面。
//
// ⚠ 本工程工具集是 VS2015(MSVC v140 / C++14), ONNX Runtime 只能用 **C API**
//   (onnxruntime_cxx_api.h 要 C++17: 无条件 include <variant> 且在函数指针 typedef 里用
//   noexcept)。见 JZCv/paddleocr/onnx_engine.cpp 顶部那段的完整说明 —— 那里有同一处补丁。
//
// 第一版刻意简单(用户要求"先简单, 后面优化"), 已知的粗糙处(留着以后改):
//   · 张量数据用 JSON 数组进出 ⇒ 大张量(几十万元素)会慢, 该改成二进制/base64 通道;
//   · 只有 CPU EP(不接 CUDA/DML/TensorRT);
//   · 输出一律转成 JS 数组返回, 没有"只取最大值的下标"这类省事的后处理。
//
// 设计(与 JZJSCvModule 同一套约定):
//   · **会话句柄**: C++ 侧一张表 map<int, OrtSession*>, JS 只拿一个 number;
//   · **失败显式**: 返回 {ok:false, error:"..."}(JS 适配层转成抛错), 原子函数用 0/空串;
//   · **缺 ORT 的构建**(JZ_ONNX 未定义): 一律报"未编入", 绝不假装成功。
//
// 接入方式(构建时补, 见 binary/code_agent/runtime 与 binary/virtual_device 的 CMakeLists):
//   m_onnxModule = std::make_unique<JZJSOnnxModule>(); m_onnxModule->initEngine(engine);
class JZFRAMEWORK_API JZJSOnnxModule : public JZObject
{
public:
    JZJSOnnxModule();                     // 定义在 .cpp: PIMPL
    ~JZJSOnnxModule();

    void initEngine(JZJSEngine* engine);
    void deinitEngine();

private:
    struct Impl;                          // 会话表; 只在 .cpp 里碰 ORT 类型(头文件不传染)
    std::unique_ptr<Impl> m_impl;
    JZJSEngine* m_engine = nullptr;

    // —— 桥接方法(参数/返回值由 jsbind 按 C++ 签名自动转换; Json::Value 自动转 JS 对象/数组) ——

    /** 本构建是否编入了 ONNX Runtime（脚本先问这一句再决定走哪条路） */
    bool        jsAvailable();
    /** ORT 版本字符串（没编入时为空串） */
    std::string jsVersion();

    /** 加载模型 -> {ok:true, session:id, inputs:[...], outputs:[...]}；失败 {ok:false,error} */
    Json::Value jsLoad(const std::string& path, int numThreads);
    /** 某个会话的输入/输出描述（名字/类型/形状） */
    Json::Value jsInfo(int id);
    /** 推理：feeds = { "输入名": {shape:[...], dtype?:'float32', data?:[...] | dataB64?:'...'} } */
    Json::Value jsRun(int id, const std::string& feedsJson);

    bool        jsRelease(int id);
    int         jsReleaseAll();
    int         jsCount();
};
