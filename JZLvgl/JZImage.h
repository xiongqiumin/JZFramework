#ifndef JZ_IMAGE_H_
#define JZ_IMAGE_H_

#include "lvgl.h"
#include <memory>
#include <string>

class JZImage
{
public:
    JZImage() = default;
    explicit JZImage(const std::string& filePath);
    JZImage(const char* data, int len);

    const lv_image_dsc_t* dsc() const;

    bool isFile() const;
    std::string filePath() const;

    int width() const;
    int height() const;

private:
    std::shared_ptr<lv_image_dsc_t> m_dsc;
    std::string m_filePath;
};

#endif
