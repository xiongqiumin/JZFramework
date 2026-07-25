#ifndef JZ_IMAGE_H_
#define JZ_IMAGE_H_

#include <memory>
#include <string>

class JZImageData
{
public:
    const void* dsc() const { return m_dsc; }
    void setDsc(const void* dsc) { m_dsc = dsc; }

private:
    const void* m_dsc = nullptr;   // 指向 lv_image_dsc_t (或文件路径)
};

class JZImage
{
public:
    JZImage() = default;
    explicit JZImage(const std::string& filePath);
    JZImage(const char* data, int len);

    const void* dsc() const;
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
