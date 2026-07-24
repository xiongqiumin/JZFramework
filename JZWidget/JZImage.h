#ifndef JZ_IMAGE_H_
#define JZ_IMAGE_H_

#include <memory>
#include <string>
class JZImage
{
public:
    JZImage() = default;
    explicit JZImage(const std::string& filePath);
    JZImage(const char* data, int len);

    const void* dsc() const;

    bool isFile() const;
    std::string filePath() const;

    int width() const;
    int height() const;

private:
    void* m_dsc = nullptr;
    std::string m_filePath;
};

#endif
