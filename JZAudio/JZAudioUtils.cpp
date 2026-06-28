#include <algorithm>
#include <cmath>
#include "string.h"
#include "JZAudioUtils.h"

using namespace std;

static std::vector<int16_t> resample(std::vector<int16_t> input, int input_sample, int output_sample)
{
    if (input.empty() || input_sample == output_sample)
    {
        return input;
    }

    // 计算输出长度
    size_t output_size = static_cast<size_t>(input.size() * static_cast<double>(output_sample) / input_sample);
    std::vector<int16_t> output(output_size, 0);

    // 修复：使用采样率计算比例（核心修复）
    const double ratio = static_cast<double>(input_sample) / output_sample;

    for (size_t i = 0; i < output_size; ++i)
    {
        // 计算在输入音频中的精确位置
        const double input_pos = static_cast<double>(i) * ratio;
        const size_t left_idx = static_cast<size_t>(std::floor(input_pos));
        const size_t right_idx = left_idx + 1;

        // 边界安全处理
        if (right_idx >= input.size())
        {
            output[i] = input[left_idx];
            continue;
        }

        // 线性插值
        const double fraction = input_pos - left_idx;
        const int16_t left_val = input[left_idx];
        const int16_t right_val = input[right_idx];

        // 修复：正确类型转换
        output[i] = static_cast<int16_t>(left_val + fraction * (right_val - left_val));
    }

    return output;
}

JZByteArray audioTrans(const JZByteArray& input_buffer, JZAudioFormat input_format, JZAudioFormat output_format)
{
    vector<int16_t> input;

    int input_gap = input_format.channelCount * 2;
    for (int i = 0; i < input_buffer.size(); i += input_gap)
    {
        const int16_t* ptr = (const int16_t*)(input_buffer.data() + i);
        input.push_back(*ptr);
    }

    vector<int16_t> out_int16 = resample(input, input_format.sampleRate, output_format.sampleRate);
    JZByteArray output;
    output.resize(out_int16.size() * 2);
    memcpy(output.data(), out_int16.data(), out_int16.size() * 2);

    // 单声道输出：直接转换为 JZByteArray
    if (output_format.channelCount == 1)
    {
        JZByteArray output;
        output.resize(out_int16.size() * 2);
        memcpy(output.data(), out_int16.data(), out_int16.size() * 2);
        return output;
    }
    // 多声道输出：将单声道数据复制到所有声道
    else
    {
        size_t sample_count = out_int16.size();
        size_t per_sample_bytes = 2;  // int16_t 占2字节
        size_t total_channels = output_format.channelCount;

        // 计算多声道总字节数
        JZByteArray multi_ch_output;
        size_t total_bytes = sample_count * total_channels * per_sample_bytes;
        multi_ch_output.resize(total_bytes);

        // 直接从 out_int16 提取数据，避免冗余的 memcpy（优化效率）
        for (size_t sample_idx = 0; sample_idx < sample_count; ++sample_idx)
        {
            // 提取当前样本的两个字节（小端序，与原始逻辑一致）
            uint8_t byte0 = (out_int16[sample_idx] >> 0) & 0xFF;
            uint8_t byte1 = (out_int16[sample_idx] >> 8) & 0xFF;

            // 复制到所有声道
            for (size_t ch = 0; ch < total_channels; ++ch)
            {
                size_t target_offset = (sample_idx * total_channels + ch) * per_sample_bytes;
                multi_ch_output[target_offset + 0] = byte0;
                multi_ch_output[target_offset + 1] = byte1;
            }
        }

        return multi_ch_output;
    }
}