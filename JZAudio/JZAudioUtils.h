#ifndef JZ_WAVE_UTILS_H_
#define JZ_WAVE_UTILS_H_

#include "JZByteArray.h"
#include "JZByteArrayView.h"
#include "JZAudioFormat.h"

// 裸 PCM 的"采样格式 + 采样率 + 声道"转换 (不做容器/编码, wav 请先 JZWaveFile::readAll)。
//
// 接口约定:
//   - 输入是 JZByteArrayView(只读视图): 不拷贝; 既有 JZByteArray 实参可隐式转换,
//     JZByteArray::fromRawData() 的借用对象也能直接传进来(零拷贝);
//   - 采样格式 UInt8 / Int16 / Int32 / Float 任意互转 (内部统一按 float [-1,1] 处理,
//     编解码缩放系数对称 128 / 32768 / 2^31 ⇒ int16,uint8 往返位精确;
//     int32 有效精度受 float 限制约 24 位);
//   - 重采样为**线性插值**(无抗混叠滤波): 语音场景够用, 高保真降采样请换更好的内核;
//   - 声道: 单声道 -> 任意 (复制)、任意 -> 单声道 (各声道平均, 真正的下混)、
//     其余情况前 min(in,out) 个直通, 多出来的输出声道用平均值填充;
//   - 参数非法 (采样率/声道 <= 0、格式未知) 或数据不足一帧时返回空数组;
//   - 输入尾部不足一帧的残字节会被丢弃 (不会越界读)。
JZFRAMEWORK_API JZByteArray audioTrans(const JZByteArrayView &input,
                                       JZAudioFormat input_format,
                                       JZAudioFormat output_format);

#endif  // JZ_WAVE_UTILS_H_
