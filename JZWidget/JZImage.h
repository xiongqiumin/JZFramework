#ifndef JZ_IMAGE_H_
#define JZ_IMAGE_H_

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

// 图像数据: RGBA8888 像素缓冲, 渲染后端 (JZImGuiBackend) 懒上传为 GPU 纹理。
class JZImageData
{
public:
    int width() const { return m_w; }
    int height() const { return m_h; }
    const uint8_t* pixels() const { return m_pixels.empty() ? nullptr : m_pixels.data(); }

    void setPixels(int w, int h, const uint8_t* rgba)
    {
        m_w = w;
        m_h = h;
        if (w > 0 && h > 0 && rgba)
            m_pixels.assign(rgba, rgba + (size_t)w * h * 4);
        else {
            m_w = 0;
            m_h = 0;
            m_pixels.clear();
        }
    }

private:
    int m_w = 0;
    int m_h = 0;
    std::vector<uint8_t> m_pixels;
};

class JZImage
{
public:
    JZImage() = default;
    explicit JZImage(const std::string& filePath);
    JZImage(const char* data, int len);

    bool isNull() const;   // 无像素数据
    std::shared_ptr<JZImageData> data() const;

    bool isFile() const;
    std::string filePath() const;

    int width() const;
    int height() const;

private:
    std::shared_ptr<JZImageData> m_data;
    std::string m_filePath;
};

#endif
