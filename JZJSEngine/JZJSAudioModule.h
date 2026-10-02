#ifndef JS_ENGINE_AUDIO_MODULE_H_
#define JS_ENGINE_AUDIO_MODULE_H_

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "JZJSEngine.h"
#include "JZByteArray.h"
#include "json/json.h"

class JZAudioInput;
class JZAudioOutput;

/**
 * JS 音频模块: 把 JZAudio(录音/放音/设备/WAV/重采样) 暴露成 `__jz.audio.*`。
 *
 * 与 JZJSCvModule / JZJSNetModule 平级: 各自 initEngine/deinitEngine, 复用全局 __jz 加子命名空间。
 *
 * 设计要点(为什么是"拉"而不是"回调"):
 *   · JZAudioInput 的数据只从 `sigReadyRead` 出来, 而它在**音频线程**上触发; QuickJS 不是线程安全的,
 *     所以这里**不在**回调里碰 JS —— 回调只把数据追加进一条带锁的缓冲, JS 侧用 readInput() 取空一块。
 *   · 想要"事件式"的用法由 JS 包装层(setInterval 拉)提供, 例如 audio.openInput().onData(cb)。
 *   · 字节一律走 Uint8Array(收 Uint8Array/Buffer/ArrayBuffer/字符串都行), 不做 base64 往返。
 *   · 不设回调, 于是也**不需要** JS 闭包的 GC 根(避免 JZJSSerialPortModule 那套 retainCallback)。
 *
 * 编入条件: HAS_JZAudio(JZFramework.cmake 里对 JZFramework 目标 PUBLIC 定义; 本机为 ON)。
 *   没编入时 available() 返回 false、调用抛 notBuilt —— 与 cv 对 OpenCV 的处理一致, 脚本好判定。
 */
class JZFRAMEWORK_API JZJSAudioModule
{
public:
    JZJSAudioModule();
    ~JZJSAudioModule();

    void initEngine(JZJSEngine* engine);
    void deinitEngine();

    // ---- 原子 API（__jz.audio.*）----
    bool available();
    Json::Value devices();                              // {inputs:[{id,name}], outputs:[{id,name}]}

    int openInput(const Json::Value& opts);             // {sampleRate,channelCount,sampleFormat,deviceId} -> id(0=失败)
    Json::Value inputInfo(int id);                      // {format..., buffered}
    JSValue readInput(int id);                          // -> Uint8Array(取空当前缓冲; 没有数据就是空数组)
    bool closeInput(int id);

    int openOutput(const Json::Value& opts);            // 同上(不含 deviceId 时用默认输出设备)
    bool writeOutput(int id, JSValue data);             // 流式喂数据(队列满返回 false)
    bool outputFinished(int id);                        // 上次播完信号之后为 true(读一次就清)
    bool closeOutput(int id);

    Json::Value wavInfo(const std::string& path);       // {channelCount,sampleRate,sampleFormat,dataSize,frames}
    JSValue wavReadAll(const std::string& path);        // -> Uint8Array(整段 PCM)
    bool wavWrite(const std::string& path, const Json::Value& fmt, JSValue data);

    JSValue transcode(JSValue data, const Json::Value& inFmt, const Json::Value& outFmt);  // = audioTrans

    int releaseAll();                                   // 关掉所有输入/输出(返回关掉的数量)

private:
    struct InputSlot
    {
        JZAudioInput* in = nullptr;
        std::mutex mu;
        JZByteArray buf;                                // 音频线程 append / 引擎线程取空
        bool overrun = false;                           // 缓冲到上限丢过数据
    };
    struct OutputSlot
    {
        JZAudioOutput* out = nullptr;
        bool finished = false;
    };

    InputSlot* findInput(int id);
    OutputSlot* findOutput(int id);
    bool dropInput(int id);
    bool dropOutput(int id);
    Json::Value notBuilt() const;

    JZJSEngine* m_engine = nullptr;
    int m_nextId = 1;
    std::map<int, InputSlot*> m_inputs;
    std::map<int, OutputSlot*> m_outputs;
};

#endif  // !JS_ENGINE_AUDIO_MODULE_H_
