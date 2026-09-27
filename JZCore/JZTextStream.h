#ifndef JZ_TEXT_STREAM_H
#define JZ_TEXT_STREAM_H
#include <string>

#include "JZFile.h"

class JZFRAMEWORK_API JZTextStream
{
public:
    enum Codec
    {
        Local,
        UTF8,
        UTF8_BOM  // 带 BOM 的 UTF8
    };

    explicit JZTextStream(JZFile* file);
    virtual ~JZTextStream();

    std::string readAll();
    std::string readLine();
    bool atEnd() const;

    // 设置编码（核心）
    void setCodec(Codec codec);
    Codec codec() const;

    // 写入 operator <<
    JZTextStream& operator<<(const std::string& str);
    JZTextStream& operator<<(JZ::EndFlag endl);

private:
    JZ_DISABLE_COPY(JZTextStream)

    static const size_t BUFFER_SIZE;

    void skipUTF8BOM();

    void writeBomIfNeeded();
    Codec m_codec;
    bool m_bomWritten;

    JZFile* m_file;
    std::string read_buffer;
    size_t buf_pos;
    std::string line_buf;
};

#endif  // JZ_TEXT_STREAM_H